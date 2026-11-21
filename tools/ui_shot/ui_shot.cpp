#include <pthread.h>
// Renders Concha's editor off screen to a PNG, with a pattern playing (free run), to check the
// UI without a DAW: concha_ui_shot <code.js> <out.png> [width height] [ms] [tab | library] [text to type at the end]
#include "input/Mouse.h"
#include "plugin/LibraryPanel.h"
#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"
#include "plugin/ui/CodeView.h"

#include <mutex>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <thread>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <code.js> <out.png> [width height] [tab]\n", argv[0]);
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    auto proc = std::make_unique<concha::ConchaProcessor>();
    proc->setPlayConfigDetails(0, 2 * concha::kNumStereoBuses, 48000, 256);
    proc->prepareToPlay(48000, 256);
    if (auto* fr = proc->state().getParameter("freeRun")) fr->setValueNotifyingHost(1.0f);
    const juce::String code = juce::File(juce::String(argv[1])).loadFileAsString();
    proc->setCode(code);
    if (argc > 6) proc->state().state.setProperty("panelTab", juce::String(argv[6]).getIntValue(), nullptr);
    if (const char* panel = std::getenv("CONCHA_SHOT_PANEL")) // 0: the side panel closed
        proc->state().state.setProperty("panelOpen", juce::String(panel).getIntValue() != 0, nullptr);

    std::atomic<bool> run{true};
    std::thread audio([&] { // the DAW's audio thread, in real time
        pthread_setname_np(pthread_self(), "host-audio");
        juce::AudioBuffer<float> buffer(2 * concha::kNumStereoBuses, 256);
        juce::MidiBuffer midi;
        auto next = std::chrono::steady_clock::now();
        float mx = -1, my = -1; // CONCHA_SHOT_MOUSE="x,y" (0..1): mousex / mousey, held there
        if (const char* m = std::getenv("CONCHA_SHOT_MOUSE")) std::sscanf(m, "%f,%f", &mx, &my);
        while (run) {
            if (mx >= 0) concha::input::setMouse(mx, my);
            proc->processBlock(buffer, midi);
            next += std::chrono::microseconds(256 * 1000000 / 48000);
            std::this_thread::sleep_until(next);
        }
    });
    std::unique_ptr<juce::AudioProcessorEditor> editor(proc->createEditor());
    const int w = argc > 4 ? juce::String(argv[3]).getIntValue() : 1100, h = argc > 4 ? juce::String(argv[4]).getIntValue() : 680;
    editor->setSize(w, h);
    // CONCHA_SHOT_GL=1: a real window (off screen) so OpenGL runs (shader, oscilloscope band); the
    // snapshot is then drawn over OpenGL's last frame
    std::mutex shaderMutex;
    juce::Image shaderFrame;
    const char* shaderOut = std::getenv("CONCHA_SHOT_GL");
    if (shaderOut) {
        if (auto* ed = dynamic_cast<concha::ConchaEditor*>(editor.get())) {
            ed->onGLFrame = [&](const juce::Image& img) {
                std::lock_guard<std::mutex> lock(shaderMutex);
                shaderFrame = img.createCopy();
            };
        }
        editor->setTopLeftPosition(-4000, -4000);
        editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);
        editor->setVisible(true);
    }
    // CONCHA_SHOT_HOSTWIN=1: the editor inside a host window that grows by itself (Bitwig maximised)
    std::unique_ptr<juce::Component> hostWindow;
    if (std::getenv("CONCHA_SHOT_HOSTWIN")) {
        hostWindow = std::make_unique<juce::Component>();
        hostWindow->setBounds(-4000, -4000, w, h);
        hostWindow->addToDesktop(juce::ComponentPeer::windowIsTemporary);
        hostWindow->setVisible(true);
        editor->addToDesktop(0, hostWindow->getPeer()->getNativeHandle());
        editor->setVisible(true);
        juce::Timer::callAfterDelay(800, [&] { hostWindow->setSize(w + 400, h + 300); });
        juce::Timer::callAfterDelay(1600, [&] {
            std::fprintf(stderr, "host window %dx%d, editor %dx%d\n", hostWindow->getWidth(), hostWindow->getHeight(), editor->getWidth(), editor->getHeight());
            hostWindow->setSize(w + 100, h + 50);
        });
        juce::Timer::callAfterDelay(2400, [&] {
            std::fprintf(stderr, "host window %dx%d, editor %dx%d\n", hostWindow->getWidth(), hostWindow->getHeight(), editor->getWidth(), editor->getHeight());
        });
    }
    // let the engine start, evaluate, play a while: the message loop runs the editor's timer
    const int playMs = argc > 5 ? juce::String(argv[5]).getIntValue() : 2500;
    const juce::String outPath(argv[2]);
    // CONCHA_SHOT_TYPING=1: while it plays, type into the code 20 times a second (live coding) and
    // report how long each key and each tick of the message thread took
    struct Typist final : juce::Timer {
        juce::Component* root = nullptr;
        concha::ui::CodeView* view = nullptr;
        int n = 0;
        double keyMs = 0, worstKeyMs = 0;
        double lastTick = 0, worstGap = 0, gaps = 0;
        int ticks = 0;
        void timerCallback() override
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (lastTick > 0) {
                const double gap = now - lastTick - 50;
                worstGap = std::max(worstGap, gap);
                gaps += std::max(0.0, gap);
                ++ticks;
            }
            lastTick = now;
            if (!view) {
                std::function<concha::ui::CodeView*(juce::Component*)> find = [&](juce::Component* c) -> concha::ui::CodeView* {
                    if (auto* v = dynamic_cast<concha::ui::CodeView*>(c)) return v;
                    for (auto* ch : c->getChildren())
                        if (auto* v = find(ch)) return v;
                    return nullptr;
                };
                view = find(root);
                if (view) view->moveCaretToEnd(false);
                return;
            }
            const char* text = " // live";
            const juce::juce_wchar ch = text[n % 8];
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            view->keyPressed(juce::KeyPress(0, {}, ch));
            const double dt = juce::Time::getMillisecondCounterHiRes() - t0;
            keyMs += dt;
            worstKeyMs = std::max(worstKeyMs, dt);
            ++n;
        }
        ~Typist() override
        {
            if (n) std::fprintf(stderr, "typing: %d keys, %.2f ms per key (worst %.1f), message thread late %.1f ms per tick (worst %.0f)\n", n, keyMs / n,
                                worstKeyMs, ticks ? gaps / ticks : 0.0, worstGap);
        }
    };
    std::unique_ptr<Typist> typist;
    juce::Timer::callAfterDelay(1500, [&] {
        proc->evaluate(code);
        // CONCHA_SHOT_ADD_FOLDER=<path>: the folder added from the panel while it plays, as a user does
        if (const char* folder = std::getenv("CONCHA_SHOT_ADD_FOLDER"))
            juce::Timer::callAfterDelay(500, [&, path = juce::String::fromUTF8(folder)] {
                if (auto* ed = dynamic_cast<concha::ConchaEditor*>(editor.get())) {
                    if (auto* panel = ed->openFolders()) panel->addFolderPath(juce::File(path));
                    juce::Timer::callAfterDelay(300, [&] {
                        if (auto* e2 = dynamic_cast<concha::ConchaEditor*>(editor.get())) e2->closeFolders();
                    });
                }
            });
        if (std::getenv("CONCHA_SHOT_TYPING")) {
            typist = std::make_unique<Typist>();
            typist->root = editor.get();
            typist->startTimer(50);
        }
        juce::Timer::callAfterDelay(playMs, [&] {
            if (argc > 7) { // type into the code view (completion check)
                std::function<concha::ui::CodeView*(juce::Component*)> find = [&](juce::Component* c) -> concha::ui::CodeView* {
                    if (auto* v = dynamic_cast<concha::ui::CodeView*>(c)) return v;
                    for (auto* ch : c->getChildren())
                        if (auto* v = find(ch)) return v;
                    return nullptr;
                };
                if (auto* view = find(editor.get())) {
                    view->moveCaretToEnd(false);
                    for (auto ch : juce::String::fromUTF8(argv[7])) view->keyPressed(juce::KeyPress(0, {}, ch));
                }
            }
            juce::Timer::callAfterDelay(400, [&] {
                // tab "library": the sample / SynthDef folders panel instead of the window
                std::unique_ptr<concha::LibraryPanel> library;
                if (argc > 6 && juce::String(argv[6]) == "library") {
                    library = std::make_unique<concha::LibraryPanel>([] {});
                    library->setSize(editor->getWidth(), editor->getHeight());
                }
                juce::Component& target = library ? static_cast<juce::Component&>(*library) : *editor;
                juce::Image shot = target.createComponentSnapshot(target.getLocalBounds(), true, 1.0f);
                if (shaderOut && !library) { // OpenGL: the shader under the components
                    std::lock_guard<std::mutex> lock(shaderMutex);
                    juce::Image both(juce::Image::ARGB, shot.getWidth(), shot.getHeight(), true);
                    juce::Graphics bg(both);
                    if (shaderFrame.isValid()) bg.drawImage(shaderFrame, both.getBounds().toFloat());
                    bg.drawImageAt(shot, 0, 0);
                    shot = both;
                }
                juce::File out{outPath};
                out.deleteFile();
                juce::FileOutputStream os{out};
                juce::PNGImageFormat().writeImageToStream(shot, os);
                typist.reset();
                juce::MessageManager::getInstance()->stopDispatchLoop();
            });
        });
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    run = false;
    audio.join();
    editor.reset();
    hostWindow.reset();
    proc.reset();
    std::printf("wrote %s\n", argv[2]);
    return 0;
}
