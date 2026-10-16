// SPDX-License-Identifier: GPL-3.0-or-later
//
// Select mode: a replacement is delivered as a Shift+Left selection request on
// the keyboard socket instead of a backspace stream. The engine commits the
// replacement once all echoed left arrows have arrived, the intermediate arrows
// pass through so the app can extend the selection, and no arrow is replayed
// afterwards.
#include "fcitx-utils/keysym.h"
#include "kb-socket-listener.h"
#include "lotus-engine.h"
#include "test-input-context.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace {

    bool receiveSelectRequest(KbSocketListener& listener, size_t& count, const char* meaning, const char* timeoutExpected = "request within 5000 ms") {
        KbMsg msg{};
        if (!listener.receive(msg, meaning, timeoutExpected))
            return false;
        if (msg.op != KbOp::Select || msg.count <= 0) {
            reportFailure("receive replacement request", "op=select, count > 0", "op=" + std::to_string(static_cast<int>(msg.op)) + ", count=" + std::to_string(msg.count),
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
                          "Select echo handling accepted or rejected the key unexpectedly");
            return false;
        }
        return true;
    }

    std::string commitList(const TestInputContext& context) {
        std::string actual = "commits=";
        for (const auto& commit : context.commits())
            actual += "['" + commit + "']";
        return actual;
    }

} // namespace

int main() {
    configureTestPaths("fcitx5-lotus-select-mode-replacement");
    TestInstance       testInstance;
    fcitx::LotusEngine engine(&testInstance.instance);
    fcitx::RawConfig   config;
    config.setValueByPath("Mode", "Uinput (Select)");
    config.setValueByPath("InputMethod", "Telex");
    engine.setConfig(config);
    if (engine.config().mode.value() != fcitx::LotusMode::Select || engine.config().inputMethod.value() != fcitx::InputMethodId::Telex) {
        reportFailure("configure Select/Telex", "mode=Select, input method=Telex", "configured mode or input method differs",
                      "the select test cannot exercise Select mode Telex behavior");
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

    // Telex a, s changes the Bamboo preedit a -> á. Select mode must ask the
    // uinput server to select the old character(s) instead of sending backspaces.
    if (!send(engine, entry, *context, FcitxKey_a, false) || !send(engine, entry, *context, FcitxKey_s, true))
        return 1;
    size_t selects = 0;
    if (!receiveSelectRequest(listener, selects, "the initial Telex replacement did not request a selection"))
        return 1;

    // A key typed while the selection is in flight must not commit immediately.
    if (!send(engine, entry, *context, FcitxKey_x, true) || !context->commits().empty()) {
        reportFailure("buffer key x before the selection completes", "no immediate commits", commitList(*context), "buffered key x was emitted before the selection completed");
        return 1;
    }

    // Echoed left arrows pass through so the app can extend the selection.
    // Pause is the anchor that commits the replacement.
    for (size_t i = 0; i < selects; ++i) {
        if (!send(engine, entry, *context, FcitxKey_Left, false))
            return 1;
    }
    if (!send(engine, entry, *context, FcitxKey_Delete, false))
        return 1;
    if (!send(engine, entry, *context, FcitxKey_Pause, true))
        return 1;

    // x typed while the selection is in flight is folded into the same commit,
    // so the selected "a" is replaced by ã and no second selection is started.
    const std::vector<std::string> expected{"ã"};
    if (context->commits() != expected) {
        reportFailure("verify folded select commit", "commits=['ã']", commitList(*context) + ", selects=" + std::to_string(selects),
                      "x typed during the selection must fold into one commit of ã");
        return 1;
    }
    return 0;
}
