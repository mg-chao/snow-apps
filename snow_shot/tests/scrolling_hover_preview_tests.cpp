#include "presentation/capture/scrollinghoverpreview.h"

#include <QCoreApplication>

#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using snow_shot::capture_detail::ScrollingHoverPreview;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QRect viewport(int top) {
    return {0, top, 400, 300};
}

QImage image(int value = 1) {
    QImage result(400, 300, QImage::Format_RGBA8888);
    result.fill(QColor(value, 2, 3));
    return result;
}

struct Harness {
    struct Request {
        QRect rect;
        std::function<void(QImage)> complete;
    };
    std::vector<std::function<void()>> pauses;
    std::deque<Request> requests;
    std::vector<std::string> events;
    int presented = 0;
    bool cropping = false;
    bool accept = true;
    ScrollingHoverPreview preview{
        {[this](std::function<void()> acknowledged) {
             events.emplace_back("pause");
             pauses.push_back(std::move(acknowledged));
         },
         [this](const QRect& rect, std::function<void(QImage)> completed) {
             if (!accept)
                 return false;
             requests.push_back({rect, std::move(completed)});
             return true;
         },
         [this](const QImage&, bool value) {
             ++presented;
             cropping = value;
             events.emplace_back("present");
         },
         [this] { events.emplace_back("clear"); }, [this] { events.emplace_back("resume"); }}};
};

void autoScrollGatesPreviewAndAcknowledgmentPrecedesExtraction() {
    Harness h;
    h.preview.setHoverRect(viewport(0));
    require(h.pauses.empty() && h.requests.empty(), "disabled hover must not pause capture");
    h.preview.setEnabled(true);
    require(h.pauses.size() == 1 && h.requests.empty(),
            "wait for source shutdown before extraction");
    h.pauses[0]();
    require(h.requests.size() == 1, "pause acknowledgment must enable extraction");
    h.requests[0].complete(image());
    h.preview.setEnabled(false);
    require(h.events == std::vector<std::string>{"pause", "present", "clear", "resume"},
            "auto-scroll enable must clear presentation before resuming capture");
}

void burstRequestsStayBoundedAndDeduplicated() {
    Harness h;
    h.preview.setEnabled(true);
    h.preview.setHoverRect(viewport(0));
    h.pauses[0]();
    for (int top = 1; top <= 10000; ++top)
        h.preview.setHoverRect(viewport(top));
    require(h.requests.size() == 1, "cursor burst may retain only one worker request");
    h.requests[0].complete(image());
    require(h.requests.size() == 2 && h.requests[1].rect == viewport(10000),
            "only the newest pending viewport should follow the in-flight request");
    h.requests[1].complete(image(2));
    h.preview.setHoverRect(viewport(10000));
    require(h.requests.size() == 2 && h.presented == 1,
            "only the latest source range may present and duplicate ranges must not re-extract");
}

void croppingPausesAutoScrollAndRefreshesPresentation() {
    Harness h;
    h.preview.setEnabled(true, false);
    h.preview.setHoverRect(viewport(0));
    require(h.pauses.empty(), "auto-scroll must suppress ordinary hover previews");
    h.preview.setHoverRect(viewport(-150), true);
    require(h.pauses.size() == 1 && h.requests.empty(),
            "cropping must pause auto-scroll before extracting an out-of-bounds viewport");
    h.pauses[0]();
    require(h.requests.size() == 1 && h.requests[0].rect == viewport(-150),
            "crop preview must retain the boundary-centered source range");
    h.requests[0].complete(image());
    require(h.cropping && h.presented == 1, "crop presentation must carry its center guide state");
    h.preview.setHoverRect(viewport(-150));
    require(!h.preview.paused() && h.events.back() == "resume",
            "releasing the crop must clear the preview before auto-scroll resumes");

    h.preview.setEnabled(true);
    h.pauses[1]();
    h.requests[1].complete(image());
    require(!h.cropping, "ordinary hover must remove the crop guide");
    h.preview.setHoverRect(viewport(-150), true);
    require(h.requests.size() == 3,
            "a crop-state change at the same source range must refresh presentation");
    h.preview.setHoverRect(viewport(-150));
    h.requests[2].complete(image());
    require(!h.cropping && h.presented == 3 && h.requests.size() == 3,
            "in-flight completion must use the latest guide state without another extraction");
}

void leaveAndReenterInvalidatePauseAndImageCallbacks() {
    Harness h;
    h.preview.setEnabled(true);
    h.preview.setHoverRect(viewport(0));
    h.preview.setHoverRect({});
    h.pauses[0]();
    require(h.requests.empty(), "leave before acknowledgment must not request an image");
    h.preview.setHoverRect(viewport(20));
    h.pauses[1]();
    h.preview.setHoverRect({});
    h.preview.setHoverRect(viewport(80));
    h.pauses[2]();
    require(h.requests.size() == 1, "re-entry must wait for old extraction to release its slot");
    h.requests[0].complete(image());
    require(h.presented == 0 && h.requests.size() == 2 && h.requests[1].rect == viewport(80),
            "stale image must be discarded and the current viewport extracted");
    h.requests[1].complete(image());
    require(h.presented == 1, "new hover session must present its image");
}

void supersededPauseAcknowledgmentsCannotUnlockNewHover() {
    Harness h;
    h.preview.setEnabled(true);
    h.preview.setHoverRect(viewport(0));
    h.preview.setEnabled(false);
    h.preview.setEnabled(true);
    h.preview.setHoverRect(viewport(40));
    h.pauses[0]();
    require(h.preview.paused() && h.requests.empty(),
            "old pause acknowledgment must not unlock a newly enabled hover");
    h.preview.setHoverRect(viewport(80));
    h.pauses[1]();
    require(h.requests.size() == 1 && h.requests[0].rect == viewport(80),
            "current pause acknowledgment must extract the newest position");

    h.preview.setEnabled(false);
    h.preview.setEnabled(true);
    h.pauses[1]();
    h.pauses[2]();
    require(h.requests.size() == 1,
            "acknowledged new hover must still wait for the canceled request slot");
    h.requests[0].complete({});
    require(h.presented == 0 && h.requests.size() == 2 && h.requests[1].rect == viewport(80),
            "empty canceled pipeline completion must release the slot and refresh current hover");
    h.requests[1].complete(image());
    require(h.presented == 1, "hover must remain usable after pause supersession");
}

void revisionChangesAndSessionResetDiscardStaleImages() {
    Harness h;
    h.preview.setEnabled(true);
    h.preview.setHoverRect(viewport(0));
    h.pauses[0]();
    h.preview.contentChanged();
    h.requests[0].complete(image());
    require(h.presented == 0 && h.requests.size() == 2,
            "committed content change must discard and refresh an in-flight viewport");
    h.preview.reset();
    h.preview.setEnabled(true);
    h.preview.setHoverRect(viewport(100));
    h.pauses[1]();
    h.requests[1].complete(image());
    require(h.presented == 0 && h.requests.size() == 3,
            "previous session callback must not free or replace the new session request");
    h.requests[2].complete(image());
    require(h.presented == 1, "new session must remain usable after a stale callback");
}

void failedExtractionKeepsCapturePausedUntilLeave() {
    Harness h;
    h.preview.setEnabled(true);
    h.preview.setHoverRect(viewport(0));
    h.pauses[0]();
    h.requests[0].complete({});
    h.preview.setHoverRect(viewport(0));
    require(h.presented == 0 && h.preview.paused() && h.requests.size() == 1,
            "null images must clear presentation without spinning or resuming capture");
    h.preview.setHoverRect({});
    require(h.events.back() == "resume", "leaving after failure must still resume capture");
    h.accept = false;
    h.preview.setHoverRect(viewport(40));
    h.pauses[1]();
    require(h.preview.paused() && h.events.back() == "clear",
            "rejected extraction must leave a recoverable paused hover");
}

void deliveryAfterOwnerDestructionIsHarmless() {
    std::function<void()> acknowledged;
    std::function<void(QImage)> completed;
    int presented = 0;
    auto preview = std::make_unique<ScrollingHoverPreview>(ScrollingHoverPreview::Context{
        [&](std::function<void()> value) { acknowledged = std::move(value); },
        [&](const QRect&, std::function<void(QImage)> value) {
            completed = std::move(value);
            return true;
        },
        [&](const QImage&, bool) { ++presented; }, [] {}, [] {}});
    preview->setEnabled(true);
    preview->setHoverRect(viewport(0));
    acknowledged();
    preview.reset();
    acknowledged();
    completed(image());
    require(presented == 0, "destroyed preview owner must reject late deliveries");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    autoScrollGatesPreviewAndAcknowledgmentPrecedesExtraction();
    burstRequestsStayBoundedAndDeduplicated();
    croppingPausesAutoScrollAndRefreshesPresentation();
    leaveAndReenterInvalidatePauseAndImageCallbacks();
    supersededPauseAcknowledgmentsCannotUnlockNewHover();
    revisionChangesAndSessionResetDiscardStaleImages();
    failedExtractionKeepsCapturePausedUntilLeave();
    deliveryAfterOwnerDestructionIsHarmless();
    std::cout << "scrolling hover preview tests passed\n";
    return 0;
}
