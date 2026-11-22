// X-Fehler loggen statt beenden (todos/2026-10-26-x-fehler-handler.md).
//
// JUCE setzt seinen X-Fehler-Handler (und XInitThreads) nur fuer eine
// JUCEApplication: XWindowSystem::XWindowSystem() fragt
// JUCEApplicationBase::isStandaloneApp() (juce_XWindowSystem_linux.cpp).
// dawd, Plugin-Host und Scan-Kind starten mit ScopedJuceInitialiser_GUI —
// ohne eigenen Handler beendete jeder X-Fehler den Prozess (Standard-Handler
// von Xlib: Meldung, exit 1), z. B. BadAtom aus XWindowSystem::isHidden ohne
// _NET_WM_STATE (Fenstermanager ohne EWMH) oder BadWindow aus einem Editor.
// JUCE selbst prueft die Rueckgabe solcher Aufrufe (isHidden: "nicht
// versteckt") — Weiterlaufen ist das, was eine JUCEApplication tut.
//
// Zugang zu Xlib wie in JUCE (juce_gui_extra.cpp, juce_opengl.cpp …):
// JUCE_GUI_BASICS_INCLUDE_XHEADERS gibt X11Symbols frei, JUCE laedt libX11
// zur Laufzeit (dlopen) — dawd linkt nichts Neues dazu. Eigene Datei, weil
// die X-Header Makros wie None/Bool/Status mitbringen.
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_events/native/juce_EventLoopInternal_linux.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <unistd.h>

namespace dawd
{
    namespace
    {
        // Wie X11ErrorHandling::errorHandler in JUCE (dort nur mit
        // JUCE_DEBUG_XERRORS und DBG, also im Release stumm): Fehler- und
        // Request-Text aus der Fehler-Datenbank von Xlib — lokal, ohne
        // Anfrage an den Server (die darf ein Handler nicht stellen).
        // Derselbe Fehler kommt oft in Serie (BadAtom aus isHidden bei jedem
        // Verschieben: ~200 in 5 s): geloggt beim 1., 10., 100. … Mal.
        int onXError (::Display* display, XErrorEvent* event)
        {
            static std::mutex lock;
            static std::map<std::pair<int, int>, long> counts;   // (Fehler, Request) -> Anzahl
            long n;

            {
                const std::lock_guard<std::mutex> l (lock);
                n = ++counts[{ (int) event->error_code, (int) event->request_code }];
            }

            auto p = n;

            while (p % 10 == 0)
                p /= 10;

            if (p != 1)   // nicht 1, 10, 100 …
                return 0;

            char errorStr[64] = {}, requestStr[64] = {};

            if (auto* x = juce::X11Symbols::getInstanceWithoutCreating())
            {
                x->xGetErrorText (display, event->error_code, errorStr, 64);
                x->xGetErrorDatabaseText (display, "XRequest", juce::String (event->request_code).toUTF8(),
                                          "Unknown", requestStr, 64);
            }

            std::cerr << "[x11 " << ::getpid() << "] X error: " << errorStr << " (code " << (int) event->error_code
                      << "), request " << requestStr << " (" << (int) event->request_code << "), ignored"
                      << (n > 1 ? " (" + std::to_string (n) + " times)" : std::string()) << "\n";
            return 0;
        }
    }

    void installXErrorHandler()
    {
        // Ohne DISPLAY oeffnet dawd keine Fenster (displayError) — dann bleibt
        // alles wie bisher, libX11 wird nicht geladen (Test-Suite: leeres DISPLAY)
        auto* d = std::getenv ("DISPLAY");

        if (d == nullptr || *d == 0)
            return;

        auto* x = juce::X11Symbols::getInstance();

        if (! x->loadAllSymbols())
            return;

        // Wie XWindowSystem fuer eine JUCEApplication: erst XInitThreads, dann
        // die Handler — vor dem ersten Xlib-Aufruf (JUCE oeffnet das Display
        // erst beim ersten Fenster). libX11 1.8.13 (hier) ruft XInitThreads
        // schon beim Laden selbst (gdb: Breakpoint in XInitThreads aus
        // _dl_init); der zweite Aufruf liefert dann nur True.
        x->xInitThreads();
        x->xSetErrorHandler (onXError);

        // IO-Fehler (Verbindung zum X-Server weg) bleiben beim Standard von
        // Xlib: Meldung "XIO: fatal IO error …", exit 1. JUCE kann sich nicht
        // neu verbinden (XWindowSystem behaelt das tote Display) und beendet
        // auch eine JUCEApplication (ioErrorHandler: "connection to X server
        // broken, terminating"); der Dienst startet mit Restart=on-failure neu.
    }

    void dispatchQueuedXEvents()
    {
        auto* ws = juce::XWindowSystem::getInstanceWithoutCreating();
        auto* x = juce::X11Symbols::getInstanceWithoutCreating();
        auto* display = ws != nullptr ? ws->getDisplay() : nullptr;

        if (x == nullptr || display == nullptr)
            return;

        // QueuedAlready: nur die Warteschlange ansehen, ohne Anfrage an den
        // Server; dann derselbe Weg wie bei lesbarer Verbindung
        if (x->xEventsQueued (display, QueuedAlready) > 0)
            juce::LinuxEventLoopInternal::invokeEventLoopCallbackForFd (x->xConnectionNumber (display));
    }
}
