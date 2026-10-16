// SPDX-License-Identifier: GPL-3.0-or-later
#include "kb-socket-listener.h"
#include "lotus-engine.h"
#include "test-input-context.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace {

    bool receiveBackspaceRequest(KbSocketListener& listener, size_t& count, const char* meaning, const char* timeoutExpected = "request within 5000 ms") {
        KbMsg msg{};
        if (!listener.receive(msg, meaning, timeoutExpected))
            return false;
        if (msg.op != KbOp::Backspace || msg.count <= 0) {
            reportFailure("receive replacement request", "op=backspace, count > 0", "op=" + std::to_string(static_cast<int>(msg.op)) + ", count=" + std::to_string(msg.count),
                          meaning);
            return false;
        }
        count = msg.count;
        return true;
    }

    bool send(fcitx::LotusEngine& engine, const fcitx::InputMethodEntry& entry, TestInputContext& context, fcitx::KeySym symbol, bool requireAccepted) {
        fcitx::KeyEvent event(&context, fcitx::Key(symbol), false);
        engine.keyEvent(entry, event);
        if (event.accepted() != requireAccepted) {
            reportFailure("process key " + std::to_string(symbol), "accepted=" + std::to_string(static_cast<int>(requireAccepted)),
                          "accepted=" + std::to_string(static_cast<int>(event.accepted())) + ", commits=" + std::to_string(context.commits().size()),
                          "Smooth buffered-key handling accepted or rejected the key unexpectedly");
            return false;
        }
        return true;
    }

} // namespace

int main() {
    configureTestPaths("fcitx5-lotus-smooth-buffered-key-replay");
    TestInstance       testInstance;
    fcitx::LotusEngine engine(&testInstance.instance);
    fcitx::RawConfig   config;
    config.setValueByPath("Mode", "Uinput (Smooth)");
    config.setValueByPath("InputMethod", "Telex");
    engine.setConfig(config);
    if (engine.config().mode.value() != fcitx::LotusMode::Smooth || engine.config().inputMethod.value() != fcitx::InputMethodId::Telex) {
        reportFailure("configure Smooth/Telex", "mode=Smooth, input method=Telex", "configured mode or input method differs",
                      "the replay test cannot exercise Smooth Telex behavior");
        return 1;
    }

    KbSocketListener listener;
    if (!listener.valid())
        return 1;
    auto context = std::make_unique<TestInputContext>(&testInstance.instance);
    context->focusIn();
    fcitx::InputMethodEntry  entry("lotus", "Lotus", "vi", "lotus");
    fcitx::InputContextEvent focus(context.get(), fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, focus);
    context->resetPreeditUpdateCount();

    // Telex rooif -> rồi. The second o starts a replacement; i and f arrive
    // before it finishes and must be folded into that commit. A second backspace
    // round deletes the word down to "r" when the intermediate commit has not landed.
    if (!send(engine, entry, *context, FcitxKey_r, false) || !send(engine, entry, *context, FcitxKey_o, false) || !send(engine, entry, *context, FcitxKey_o, true))
        return 1;
    size_t backspaces = 0;
    if (!receiveBackspaceRequest(listener, backspaces, "the oo replacement request did not arrive"))
        return 1;
    if (!send(engine, entry, *context, FcitxKey_i, true) || !send(engine, entry, *context, FcitxKey_f, true) || !context->commits().empty()) {
        reportFailure("buffer i and f before deletion completes", "no immediate commits", "commits=" + std::to_string(context->commits().size()),
                      "keys typed during the oo replacement were emitted before it completed");
        return 1;
    }
    for (size_t i = 0; i < backspaces; ++i) {
        if (!send(engine, entry, *context, FcitxKey_BackSpace, i + 1 == backspaces))
            return 1;
    }
    const std::vector<std::string> roi{"ồi"};
    if (context->commits() != roi) {
        std::string actual = "commits=";
        for (const auto& commit : context->commits())
            actual += "['" + commit + "']";
        reportFailure("verify rooif folded commit", "commits=['ồi']", actual + ", backspaces=" + std::to_string(backspaces),
                      "fast Telex rooif must commit ồi once, after the single oo backspace, so the word is rồi");
        return 1;
    }

    // Telex a, s changes a -> á. x typed during that replacement folds into ã.
    if (!send(engine, entry, *context, FcitxKey_Escape, false) || !send(engine, entry, *context, FcitxKey_a, false) || !send(engine, entry, *context, FcitxKey_s, true))
        return 1;
    size_t toneBackspaces = 0;
    if (!receiveBackspaceRequest(listener, toneBackspaces, "the a/s replacement request did not arrive"))
        return 1;
    if (!send(engine, entry, *context, FcitxKey_x, true))
        return 1;
    for (size_t i = 0; i < toneBackspaces; ++i) {
        if (!send(engine, entry, *context, FcitxKey_BackSpace, i + 1 == toneBackspaces))
            return 1;
    }
    const std::vector<std::string> expected{"ồi", "ã"};
    if (context->commits() != expected) {
        std::string actual = "commits=";
        for (const auto& commit : context->commits())
            actual += "['" + commit + "']";
        reportFailure("verify folded tone commit", "commits=['ồi']['ã']", actual, "x typed during a -> á must fold into a single commit of ã");
        return 1;
    }

    // Chromium reports inline completion with the caret at the end of the
    // suggestion (cursor) and the anchor where the typed word ends. "go" then
    // the second o must delete the typed o and the selected tail.
    auto expectGoogleBackspaces = [&](const char* label, const std::string& text, unsigned int cursor, unsigned int anchor, size_t expectedCount) {
        fcitx::KeyEvent escapeEvent(context.get(), fcitx::Key(FcitxKey_Escape), false);
        engine.keyEvent(entry, escapeEvent);
        if (escapeEvent.accepted()) {
            reportFailure(label, "Escape resets composition", "Escape was accepted", "a finished replacement must not still be swallowing keys");
            return false;
        }
        if (!send(engine, entry, *context, FcitxKey_g, false) || !send(engine, entry, *context, FcitxKey_o, false))
            return false;
        context->setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
        context->surroundingText().setText(text, cursor, anchor);
        context->updateSurroundingText();
        if (!send(engine, entry, *context, FcitxKey_o, true))
            return false;
        size_t count = 0;
        if (!receiveBackspaceRequest(listener, count, label))
            return false;
        if (count != expectedCount) {
            reportFailure(label, "backspaces=" + std::to_string(expectedCount), "backspaces=" + std::to_string(count),
                          "the address bar eats the first Backspace to clear inline completion; a plain field must keep the normal count");
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            if (!send(engine, entry, *context, FcitxKey_BackSpace, i + 1 == count))
                return false;
        }
        return true;
    };
    if (!expectGoogleBackspaces("caret at end of suggestion", "google.com", 10, 2, 3))
        return 1;
    if (!expectGoogleBackspaces("caret at start of suggestion", "google.com", 2, 10, 3))
        return 1;
    if (!expectGoogleBackspaces("plain field with no suggestion", "go", 2, 2, 2))
        return 1;

    // Stale selection from an older prefix must not add a backspace: the
    // typed word is no longer the text in front of that selection.
    if (!send(engine, entry, *context, FcitxKey_Escape, false) || !send(engine, entry, *context, FcitxKey_c, false) || !send(engine, entry, *context, FcitxKey_h, false) ||
        !send(engine, entry, *context, FcitxKey_u, false))
        return 1;
    context->surroundingText().setText("chrome://gpu", 2, 12);
    context->updateSurroundingText();
    if (!send(engine, entry, *context, FcitxKey_s, true))
        return 1;
    size_t staleCount = 0;
    if (!receiveBackspaceRequest(listener, staleCount, "stale suggestion must not add a backspace"))
        return 1;
    if (staleCount != 2) {
        reportFailure("stale suggestion", "backspaces=2", "backspaces=" + std::to_string(staleCount),
                      "a selection that no longer matches the typed word must not delete the character before the mark");
        return 1;
    }
    for (size_t i = 0; i < staleCount; ++i) {
        if (!send(engine, entry, *context, FcitxKey_BackSpace, i + 1 == staleCount))
            return 1;
    }

    // Chrome and GNOME Wayland often send no surrounding text for the address
    // bar. "oo" then needs one extra real backspace so the eaten key does not
    // leave the first o in front of ô.
    auto expectAddressBarOo = [&](const char* label, TestInputContext& addressContext) {
        if (!send(engine, entry, addressContext, FcitxKey_o, false) || !send(engine, entry, addressContext, FcitxKey_o, true))
            return false;
        if (!addressContext.commits().empty()) {
            std::string actual = "commits=";
            for (const auto& commit : addressContext.commits())
                actual += "['" + commit + "']";
            reportFailure(label, "no commit yet", actual, "the address bar must not commit a placeholder before the backspaces");
            return false;
        }
        size_t addressBackspaces = 0;
        if (!receiveBackspaceRequest(listener, addressBackspaces, label))
            return false;
        if (addressBackspaces != 3) {
            reportFailure(label, "backspaces=3", "backspaces=" + std::to_string(addressBackspaces),
                          "one real backspace clears inline completion and another deletes the typed o, plus the swallowed signal");
            return false;
        }
        for (size_t i = 0; i < addressBackspaces; ++i) {
            if (!send(engine, entry, addressContext, FcitxKey_BackSpace, i + 1 == addressBackspaces))
                return false;
        }
        const std::vector<std::string> addressExpected{"ô"};
        if (addressContext.commits() != addressExpected) {
            std::string actual = "commits=";
            for (const auto& commit : addressContext.commits())
                actual += "['" + commit + "']";
            reportFailure(label, "commits=['ô']", actual, "oo in the address bar must end as ô");
            return false;
        }
        return true;
    };
    auto chromeContext = std::make_unique<TestInputContext>(&testInstance.instance, "chrome");
    chromeContext->focusIn();
    fcitx::InputContextEvent chromeFocus(chromeContext.get(), fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, chromeFocus);
    if (!expectAddressBarOo("Chrome oo without surrounding text", *chromeContext))
        return 1;

    // A Chrome field that already contains text before the word is not the address bar.
    if (!send(engine, entry, *chromeContext, FcitxKey_Escape, false) || !send(engine, entry, *chromeContext, FcitxKey_t, false) ||
        !send(engine, entry, *chromeContext, FcitxKey_o, false))
        return 1;
    chromeContext->setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
    chromeContext->surroundingText().setText("hello to", 8, 8);
    chromeContext->updateSurroundingText();
    const auto commitsBefore = chromeContext->commits().size();
    if (!send(engine, entry, *chromeContext, FcitxKey_o, true))
        return 1;
    if (chromeContext->commits().size() != commitsBefore) {
        reportFailure("plain Chrome field", "no extra commit", "commits grew", "a longer plain field must keep a single real backspace");
        return 1;
    }
    size_t plainBackspaces = 0;
    if (!receiveBackspaceRequest(listener, plainBackspaces, "the plain Chrome field replacement did not arrive"))
        return 1;
    if (plainBackspaces != 2) {
        reportFailure("plain Chrome field backspaces", "backspaces=2", "backspaces=" + std::to_string(plainBackspaces),
                      "a longer plain field keeps a single real backspace");
        return 1;
    }
    for (size_t i = 0; i < plainBackspaces; ++i) {
        if (!send(engine, entry, *chromeContext, FcitxKey_BackSpace, i + 1 == plainBackspaces))
            return 1;
    }

    // GNOME Wayland creates one ibus context named gnome-shell and the app rule
    // forces SuperSmooth, which used to skip the extra backspace entirely.
    fcitx::RawConfig superConfig;
    superConfig.setValueByPath("Mode", "Uinput (Super Smooth)");
    superConfig.setValueByPath("InputMethod", "Telex");
    engine.setConfig(superConfig);
    if (engine.config().mode.value() != fcitx::LotusMode::SuperSmooth) {
        reportFailure("switch to SuperSmooth", "mode=SuperSmooth", "mode stayed Smooth", "the gnome-shell address bar runs in SuperSmooth");
        return 1;
    }
    auto shellContext = std::make_unique<TestInputContext>(&testInstance.instance, "gnome-shell");
    shellContext->focusIn();
    fcitx::InputContextEvent shellFocus(shellContext.get(), fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, shellFocus);
    if (!expectAddressBarOo("gnome-shell SuperSmooth oo", *shellContext))
        return 1;
    return 0;
}
