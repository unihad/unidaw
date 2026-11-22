# dawd wird Open Source (AGPLv3), Einnahmen nur über Spenden

- **Status:** gilt
- **Datum:** 2026-11-18, Matthias
- **Ersetzt:** [2026-10-24 Vertrieb ruht bis zum ausgereiften Produkt](2026-10-24-vertrieb-ruht.md)
  und die Entscheidung „Closed-Source-Desktop-App“ in
  [`docs/vertrieb-und-verkauf.md`](../vertrieb-und-verkauf.md)

## Kontext

Bis hier galt: dawd wird als Closed-Source-Desktop-App verkauft
(`docs/vertrieb-und-verkauf.md`), der Vertrieb ruht aber, bis das Produkt
ausgereift ist ([2026-10-24](2026-10-24-vertrieb-ruht.md)).

Am 2026-11-18 hat Matthias das Ziel „agent daw bereit für den Vertrieb
machen“ in der internen Planung angelegt. Begründung dort: unihad ist ein
Ein-Mann-Unternehmen, Support ist nicht leistbar, und Verträge mit Kunden
sollen vorerst wegfallen. Dafür passt Open Source mit Spenden besser als
ein Verkauf.

Die Lizenzen der Basis lassen das zu:

- JUCE: AGPLv3 oder kommerzielle Lizenz
  (`tracktion_engine/modules/juce/LICENSE.md`).
- Tracktion Engine: GPL3 (oder später) oder kommerzielle Lizenz
  (`tracktion_engine/LICENSE.md`).

## Entscheidung

Antworten von Matthias, 2026-11-18:

- **Lizenz:** dawd erscheint unter AGPLv3. Genauer AGPL-3.0-only, weil
  JUCE „AGPLv3“ ohne „or later“ nennt (`docs/lizenzen-agpl.md`).
- **Einnahmen:** nur Spenden. Kein Verkauf, kein Support, keine Verträge
  mit Kunden.
- **Erste Version:** nur Linux, als AppImage im GitHub-Release.
- **Code:** öffentlich auf GitHub. Das Arbeits-Repo bleibt auf Forgejo.
  Matthias sagte zuerst „Spiegel“; seit
  [2026-11-18 Öffentlicher Stand ohne Git-Historie](2026-11-18-oeffentlicher-stand-ohne-historie.md)
  ist es ein Export des Baums von main ohne Historie und ohne interne
  Pfade, kein Spiegel.
- **Beiträge:** Issues sind offen. Pull Requests von außen werden nicht
  angenommen. Es gibt keine Zusage, auf Issues zu antworten.

## Folgen

- Der Plan „Closed-Source-Desktop-App mit Einmalkauf“ gilt nicht mehr. In
  `docs/vertrieb-und-verkauf.md` steht er nur noch als früherer Plan.
- Wegen der AGPLv3 braucht dawd die kommerziellen Lizenzen von JUCE und
  Tracktion Engine nicht. Die übrigen Bausteine sind geprüft: alles in und
  neben der Binary ist mit AGPLv3 vereinbar, Modelle liefert das AppImage
  nicht mit (`docs/lizenzen-agpl.md`). Lizenztext: `LICENSE`, Drittlizenzen:
  `THIRD_PARTY_LICENSES.md`.
- Aus den zehn Arbeitspaketen fallen weg: Portierung auf macOS und Windows
  (8), Lizenzprüfung und Testversion (9), Shop, AGB und EULA (Teil von 10).
- Arbeit bis zum ersten Release steht als Todos im Repo (Ziel
  `agent-daw-bereit-fuer-den-vertrieb-machen`):
  - `todos/2026-11-18-lizenzen-der-abhaengigkeiten-fuer-agpl-pruefen.md`
  - `todos/2026-11-18-repo-fuer-veroeffentlichung-pruefen.md`
  - `todos/2026-11-18-oeffentlichen-stand-bereinigen.md`
  - `todos/2026-11-18-lizenzhinweise-und-quellcode-link-in-der-ui.md`
  - `todos/2026-11-18-localhost-api-absichern.md`
  - `todos/2026-11-18-ui-und-ressourcen-ohne-quellordner.md`
  - `todos/2026-11-18-appimage-bauen.md`
  - `todos/2026-11-18-readme-und-projektdateien-fuer-github.md`
  - `todos/2026-11-18-mcp-server-fuer-agenten-veroeffentlichen.md`
  - `todos/2026-11-18-demo-video-aufnehmen.md`
  - `todos/2026-11-18-erstes-release-veroeffentlichen.md`
- Außerhalb des Repos (Ziel in der internen Planung): GitHub-Organisation mit
  Sponsors, Export nach GitHub, Namensprüfung „dawd“, Spenden steuerlich
  klären.
- `APP.md` bleibt bei `audience: external` und `launch: development`, bis
  das erste Release erscheint.
