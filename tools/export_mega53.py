"""MVSep Mega 53 (BS-RoFormer) nach ONNX, Schnittstelle wie htdemucs.onnx in dawd.

Aufruf: MSST_DIR=<Music-Source-Separation-Training> python3 -I tools/export_mega53.py <ckpt> <yaml> <out.onnx> [N]
(tools/fetch_ml.sh mit MEGA53=1 laedt Checkpoint, Config und MSST-Code und ruft es auf.)
Braucht torch, einops und onnx; ~8 GB RAM, ~2 min.

Eingang  mix:   float32 (1, 2, N)       N = audio.chunk_size der Config (441000)
Ausgang  stems: float32 (1, 53, 2, N)   Reihenfolge = training.instruments

STFT und iSTFT laufen im Graphen ohne komplexe Tensoren: Faltung mit
DFT-Basis (Hann-Fenster, center mit reflect-Padding wie torch.stft) und
transponierte Faltung mit Fensterquadrat-Normierung (wie torch.istft).
"""
import os, sys, math

# Modellcode aus MSST (ZFTurbo/Music-Source-Separation-Training, MIT)
sys.path.insert(0, os.environ['MSST_DIR'])

import torch
import torch.nn as nn
import torch.nn.functional as F
from einops import rearrange, pack, unpack


def load_model(ckpt, yaml_path):
    from utils.settings import get_model_from_config
    model, config = get_model_from_config('bs_roformer', yaml_path)
    sd = torch.load(ckpt, map_location='cpu', weights_only=False)
    for k in ('state', 'state_dict', 'model_state_dict'):
        if isinstance(sd, dict) and k in sd:
            sd = sd[k]
    model.load_state_dict(sd)
    model.eval()
    return model, config


class Mega53Onnx(nn.Module):
    def __init__(self, model, length):
        super().__init__()
        self.m = model
        kw = model.stft_kwargs
        assert not kw['normalized']
        self.n_fft, self.hop, self.win = kw['n_fft'], kw['hop_length'], kw['win_length']
        assert self.n_fft == self.win
        self.length = length
        n = torch.arange(self.n_fft, dtype=torch.float64)
        k = torch.arange(self.n_fft // 2 + 1, dtype=torch.float64)
        w = torch.hann_window(self.win, periodic=True, dtype=torch.float64)
        ang = 2 * math.pi * k[:, None] * n[None, :] / self.n_fft
        # Analyse: X_k = sum_n x[n] w[n] e^{-j ang}
        fwd = torch.cat([torch.cos(ang) * w, -torch.sin(ang) * w], 0)          # (2K, N)
        self.register_buffer('fwd', fwd.float()[:, None, :])
        # Synthese (irfft, einseitig): Gewichte 1 für DC/Nyquist, sonst 2; Imag von DC/Nyquist zählt nicht
        c = torch.full_like(k, 2.0); c[0] = 1.0; c[-1] = 1.0
        inv_re = (c[:, None] * torch.cos(ang)) / self.n_fft * w
        inv_im = (-c[:, None] * torch.sin(ang)) / self.n_fft * w
        inv_im[0] = 0; inv_im[-1] = 0
        self.register_buffer('inv', torch.cat([inv_re, inv_im], 0).float()[:, None, :])   # (2K, 1, N)
        frames = 1 + length // self.hop
        total = (frames - 1) * self.hop + self.n_fft
        env = torch.zeros(total, dtype=torch.float64)
        for t in range(frames):
            env[t * self.hop:t * self.hop + self.n_fft] += w ** 2
        pad = self.n_fft // 2
        env = env[pad:pad + length]
        self.register_buffer('inv_env', (1.0 / env.clamp_min(1e-11)).float())
        self.frames = frames
        self.K = self.n_fft // 2 + 1

    def stft(self, x):                     # (B, T) -> (B, K, F, 2)
        pad = self.n_fft // 2
        x = F.pad(x[:, None, :], (pad, pad), mode='reflect')
        y = F.conv1d(x, self.fwd, stride=self.hop)                      # (B, 2K, F)
        return torch.stack([y[:, :self.K], y[:, self.K:]], -1)

    def istft(self, s):                    # (B, K, F, 2) -> (B, T)
        y = torch.cat([s[..., 0], s[..., 1]], 1)                        # (B, 2K, F)
        x = F.conv_transpose1d(y, self.inv, stride=self.hop)[:, 0]      # (B, (F-1)hop+N)
        pad = self.n_fft // 2
        return x[:, pad:pad + self.length] * self.inv_env

    def forward(self, mix):
        m = self.m
        b = mix.shape[0]
        raw = mix.reshape(b * 2, self.length)
        st = self.stft(raw)                                             # (b*s, f, t, c)
        st = st.reshape(b, 2, self.K, self.frames, 2)
        stft_repr = rearrange(st, 'b s f t c -> b (f s) t c')
        x = rearrange(stft_repr, 'b f t c -> b t (f c)')
        x = m.band_split(x)
        for blk in m.layers:
            if len(blk) == 3:
                lin, time_t, freq_t = blk
                x, ps = pack([x], 'b * d'); x = lin(x); x, = unpack(x, ps, 'b * d')
            else:
                time_t, freq_t = blk
            x = rearrange(x, 'b t f d -> b f t d')
            x, ps = pack([x], '* t d'); x = time_t(x); x, = unpack(x, ps, '* t d')
            x = rearrange(x, 'b f t d -> b t f d')
            x, ps = pack([x], '* f d'); x = freq_t(x); x, = unpack(x, ps, '* f d')
        x = m.final_norm(x)
        mask = torch.stack([fn(x) for fn in m.mask_estimators], dim=1)
        mask = rearrange(mask, 'b n t (f c) -> b n f t c', c=2)
        sr = stft_repr[:, None]                                         # (b, 1, fs, t, c)
        re = sr[..., 0] * mask[..., 0] - sr[..., 1] * mask[..., 1]
        im = sr[..., 0] * mask[..., 1] + sr[..., 1] * mask[..., 0]
        out = torch.stack([re, im], -1)                                 # (b, n, fs, t, c)
        n = out.shape[1]
        out = rearrange(out, 'b n (f s) t c -> (b n s) f t c', s=2)
        if m.zero_dc:
            keep = torch.ones(self.K, 1, 1, device=out.device); keep[0] = 0
            out = out * keep
        audio = self.istft(out)
        return audio.reshape(b, n, 2, self.length)


def main():
    ckpt, yaml_path, out = sys.argv[1:4]
    model, config = load_model(ckpt, yaml_path)
    length = int(sys.argv[4]) if len(sys.argv) > 4 else int(config.audio.chunk_size)
    wrap = Mega53Onnx(model, length).eval()
    dummy = torch.randn(1, 2, length) * 0.1
    # > 2 GB: der Export legt die Gewichte als viele Einzeldateien ab; danach
    # in eine Datei <out>.data neben <out> zusammenfassen
    import tempfile, shutil, onnx
    tmp = tempfile.mkdtemp(dir=os.path.dirname(os.path.abspath(out)))
    try:
        raw = os.path.join(tmp, 'raw.onnx')
        with torch.inference_mode():
            torch.onnx.export(wrap, (dummy,), raw, input_names=['mix'], output_names=['stems'],
                              opset_version=17, dynamo=False, do_constant_folding=True)
        proto = onnx.load(raw, load_external_data=True)
        data = os.path.basename(out) + '.data'
        for p in (out, out + '.data'):
            if os.path.exists(p): os.remove(p)
        onnx.save_model(proto, out, save_as_external_data=True, all_tensors_to_one_file=True,
                        location=data, size_threshold=1024)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('written', out, os.path.getsize(out), out + '.data', os.path.getsize(out + '.data'))


if __name__ == '__main__':
    main()
