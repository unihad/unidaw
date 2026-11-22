#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <atomic>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

struct HttpReply
{
    juce::String contentType;
    std::string body;
    int status = 200;
};

// Annehmen und Lesen der Anfragen laufen auf dem Server-Thread, einer nach dem
// anderen — handle sieht die Anfragen in Eingangsreihenfolge (Befehle werden
// darin in die Message-Queue gestellt, die Reihenfolge bleibt wie frueher).
// handle gibt eine Fortsetzung zurueck, die auf einem eigenen Thread je
// Verbindung die Antwort baut (auf den Message-Thread warten, Dateien lesen).
// So beantwortet der Server GET /progress, /health oder /audio, waehrend ein
// langer Befehl (treeCheckout laedt Plugins) den Message-Thread haelt.
class HttpServer
{
public:
    using Reply = std::function<HttpReply()>;

    // Body als rohe Bytes: /upload traegt Binaerdateien (Audio), die duerfen
    // nicht durch eine UTF-8-Konvertierung. /command dekodiert selbst.
    std::function<Reply (const juce::String& method, const juce::String& path, const std::string& body)> handle;

    // Schutz der localhost-API (ARCHITECTURE.md, localhost-API absichern):
    // Host nur 127.0.0.1:<port>/localhost:<port> (gegen DNS-Rebinding),
    // Origin nur die eigene UI oder keiner (Skripte, MCP), jeder POST mit
    // dem Token der Sitzung (Authorization: Bearer <token>)
    std::string token;
    juce::String tokenFile;   // nur fuer die Fehlermeldung

    bool start (int listenPort)
    {
        port = listenPort;
        return socket.createListener (port, "127.0.0.1");
    }

    void run()
    {
        while (! quit)
        {
            std::shared_ptr<juce::StreamingSocket> client (socket.waitForNextConnection());

            if (client == nullptr)
                break;

            auto reply = readOne (*client);

            if (! reply)
                continue;

            auto count = active;
            ++*count;
            std::thread ([count, client, reply]
            {
                auto r = reply();
                // no-store: Firefox legte sonst jede POST-Antwort als eigenen
                // Cache-Eintrag ab — die UI fragt ~14x/s ab, nach Tagen 400 000
                // Eintraege (2,2 GB), der Cache-Thread lief dauernd voll und
                // hielt jede Anfrage der Seite auf (2026-10-20)
                auto statusLine = r.status == 401 ? std::string ("401 Unauthorized")
                                : r.status == 403 ? std::string ("403 Forbidden")
                                : std::string ("200 OK");
                auto head = "HTTP/1.1 " + statusLine + "\r\nContent-Type: " + r.contentType.toStdString()
                            + "\r\nContent-Length: " + std::to_string (r.body.size())
                            + "\r\nCache-Control: no-store"
                            + "\r\nConnection: close\r\n\r\n";
                client->write (head.data(), (int) head.size());
                client->write (r.body.data(), (int) r.body.size());
                --*count;
            }).detach();
        }
    }

    // Laufende Antworten haben bis zu waitMs Zeit (ein Befehl, der noch auf
    // den beendeten Message-Thread wartet, kommt nicht mehr zurueck)
    void stop (int waitMs = 3000)
    {
        quit = true;
        socket.close();

        for (int i = 0; i < waitMs / 10 && *active > 0; ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

private:
    Reply readOne (juce::StreamingSocket& client)
    {
        std::string request;
        char buffer[4096];
        size_t headerEnd = std::string::npos;
        int emptyReads = 0;

        while (headerEnd == std::string::npos)
        {
            auto n = readSome (client, buffer);

            if (n < 0)
                return {};

            if (n == 0)
            {
                if (++emptyReads > 50)
                    return {};

                continue;
            }

            emptyReads = 0;
            request.append (buffer, (size_t) n);
            headerEnd = request.find ("\r\n\r\n");
        }

        auto bodyLength = contentLength (request.substr (0, headerEnd));

        while (request.size() < headerEnd + 4 + bodyLength)
        {
            auto n = readSome (client, buffer);

            if (n < 0)
                return {};

            if (n == 0)
            {
                if (++emptyReads > 50)
                    return {};

                continue;
            }

            emptyReads = 0;
            request.append (buffer, (size_t) n);
        }

        auto body = request.substr (headerEnd + 4, bodyLength);
        auto firstLineEnd = request.find ("\r\n");
        auto firstLine = request.substr (0, firstLineEnd);
        auto space = firstLine.find (' ');
        auto pathEnd = firstLine.find (' ', space + 1);
        auto method = firstLine.substr (0, space);
        auto path = firstLine.substr (space + 1, pathEnd - space - 1);
        auto headers = request.substr (firstLineEnd, headerEnd - firstLineEnd);

        if (auto refusal = refuse (method, headers))
        {
            std::cerr << "[server] refused " << method << " " << path << ": " << refusal->body << "\n";
            return [r = *refusal] { return r; };
        }

        return handle (juce::String (method.c_str()), juce::String (path.c_str()), body);
    }

    // Abweisung oder nichts — vor jeder Route, auch vor GET /ui (die Seite
    // traegt den Token)
    std::optional<HttpReply> refuse (const std::string& method, const std::string& headers) const
    {
        auto fail = [] (int status, const juce::String& error)
        {
            auto o = new juce::DynamicObject();
            o->setProperty ("ok", false);
            o->setProperty ("error", error);
            return HttpReply { "application/json", juce::JSON::toString (juce::var (o), true).toStdString(), status };
        };

        const auto p = std::to_string (port);
        auto host = header (headers, "host");

        if (! host || (*host != "127.0.0.1:" + p && lower (*host) != "localhost:" + p))
            return fail (403, "forbidden host: unidaw answers only to Host 127.0.0.1:" + juce::String (port)
                                  + " or localhost:" + juce::String (port));

        auto origin = header (headers, "origin");

        if (origin && *origin != "http://127.0.0.1:" + p && lower (*origin) != "http://localhost:" + p)
            return fail (403, "forbidden origin: only the unidaw UI (http://127.0.0.1:" + juce::String (port)
                                  + ") or no Origin (scripts, MCP)");

        if (method == "POST")
        {
            auto auth = header (headers, "authorization");
            const std::string prefix = "Bearer ";

            if (token.empty() || ! auth || auth->compare (0, prefix.size(), prefix) != 0
                || ! sameToken (auth->substr (prefix.size()), token))
                return fail (401, "missing or wrong token: send the header Authorization: Bearer <token>, "
                                  "the token is in " + tokenFile + " (or UNIDAW_TOKEN)");
        }

        return std::nullopt;
    }

    // Vergleich ohne fruehen Abbruch (Laufzeit verraet keine Stelle)
    static bool sameToken (const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;

        unsigned char diff = 0;

        for (size_t i = 0; i < a.size(); ++i)
            diff |= (unsigned char) (a[i] ^ b[i]);

        return diff == 0;
    }

    static std::string lower (std::string s)
    {
        std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        return s;
    }

    // Wert des ersten Headers name (klein geschrieben), ohne Leerraum am Rand
    static std::optional<std::string> header (const std::string& headers, const std::string& name)
    {
        size_t pos = 0;

        while (pos < headers.size())
        {
            auto end = headers.find ("\r\n", pos);

            if (end == std::string::npos)
                end = headers.size();

            auto line = headers.substr (pos, end - pos);
            auto colon = line.find (':');

            if (colon != std::string::npos && lower (line.substr (0, colon)) == name)
            {
                auto v = line.substr (colon + 1);
                auto b = v.find_first_not_of (" \t");
                auto e = v.find_last_not_of (" \t");
                return b == std::string::npos ? std::string() : v.substr (b, e - b + 1);
            }

            pos = end + 2;
        }

        return std::nullopt;
    }

    static int readSome (juce::StreamingSocket& client, char* buffer)
    {
        if (client.waitUntilReady (true, 10000) != 1)
            return -1;

        auto n = client.read (buffer, 4096, false);

        if (n < 0)
            return -1;

        return n;
    }

    // Header-Namen sind case-insensitiv (Node-fetch schickt content-length)
    static size_t contentLength (const std::string& headers)
    {
        std::string lower (headers);
        std::transform (lower.begin(), lower.end(), lower.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        auto pos = lower.find ("content-length:");

        if (pos == std::string::npos)
            return 0;

        return (size_t) std::stoul (headers.substr (pos + 15));
    }

    juce::StreamingSocket socket;
    int port = 0;
    std::atomic<bool> quit { false };
    // Antwort-Threads, die noch schreiben (geteilt: ein Thread kann den
    // Server ueberleben, wenn stop nicht auf ihn wartet)
    std::shared_ptr<std::atomic<int>> active = std::make_shared<std::atomic<int>> (0);
};
