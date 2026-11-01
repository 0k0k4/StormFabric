#include "../src/custom_display_targets.h"
#include <cassert>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>
#include <vector>

using namespace swml;
using Result = CustomDisplayQueueResult;
namespace {
struct State {
    uint32_t width = 1, height = 1, scale = 1;
    CustomDisplayResourceHandle video, overlay;
    CustomDisplayTargetView view() { return {width, height, scale, video, overlay}; }
};
struct Object { bool ready = false, destroyed = false; };
struct Event {
    bool destroy;
    int32_t width, height, format, flag;
    CustomDisplayResourceHandle* handle;
    Object* object;
};
struct Queue {
    State* state;
    std::deque<Event> events;
    std::vector<std::unique_ptr<Object>> objects;
};
void Request(void* context, int32_t width, int32_t height, int32_t format, int32_t flag,
             CustomDisplayResourceHandle* handle) {
    auto& q = *static_cast<Queue*>(context);
    assert(!handle->object && !handle->manager);
    assert(handle == &q.state->video || handle == &q.state->overlay);
    assert(q.state->width > 0 && q.state->height > 0 && q.state->scale > 0);
    const bool video = handle == &q.state->video;
    assert(uint32_t(width) == q.state->width * (video ? q.state->scale : 1));
    assert(uint32_t(height) == q.state->height * (video ? q.state->scale : 1));
    assert(format == 5 && flag == 1);
    q.objects.emplace_back(new Object);
    auto* object = q.objects.back().get();
    handle->object = object;
    handle->manager = context;
    q.events.push_back({false, width, height, format, flag, handle, object});
}
// Test double of the observed stock destroy contract. Nothing below runs native
// code, frees a game allocation, creates a GL context or claims GPU validation.
void Release(Queue& q) {
    for (auto* handle : {&q.state->video, &q.state->overlay}) {
        if (handle->object) {
            assert(handle->manager == &q);
            q.events.push_back({true, 0, 0, 0, 0, handle, static_cast<Object*>(handle->object)});
            *handle = {};
        }
    }
}
void Drain(Queue& q) {
    for (const auto& event : q.events) {
        assert(!event.object->destroyed);
        if (event.destroy) event.object->destroyed = true;
        else event.object->ready = true;
    }
    q.events.clear();
}
bool Same(const State& a, const State& b) {
    return a.width == b.width && a.height == b.height && a.scale == b.scale &&
        a.video.object == b.video.object && a.video.manager == b.video.manager &&
        a.overlay.object == b.overlay.object && a.overlay.manager == b.overlay.manager;
}
}

int main() {
    // Explicit TEST policy, not certified game/hardware limits.
    const CustomDisplayLimits limits{16, 16, 4096, 4096, 64 * 1024 * 1024};
    const CustomDisplaySpec spec{2, 1, 480, 272};
    State state;
    Queue queue{&state, {}, {}};
    const auto initial = state;
    auto run = [&](int32_t quality) {
        return QueueCustomDisplayTargets(spec, limits, quality, &queue, &Request, state.view());
    };
    assert(CustomDisplaySupersamplingForQuality(1) == 1);
    assert(CustomDisplaySupersamplingForQuality(3) == 4);
    for (int32_t quality : {0, 2, 4, -1, (std::numeric_limits<int32_t>::min)(),
                           (std::numeric_limits<int32_t>::max)()})
        assert(CustomDisplaySupersamplingForQuality(quality) == 2);
    // Null backends and all invalid input leave every field/handle unchanged.
    assert(QueueCustomDisplayTargets(spec, limits, 1, nullptr, &Request, state.view()) == Result::MissingBackend);
    assert(QueueCustomDisplayTargets(spec, limits, 1, &queue, nullptr, state.view()) == Result::MissingBackend);
    assert(QueueCustomDisplayTargets(spec, limits, 1, &queue, &Request,
        {state.width, state.width, state.scale, state.video, state.overlay}) == Result::InvalidView);
    assert(QueueCustomDisplayTargets(spec, limits, 1, &queue, &Request,
        {state.width, state.height, state.width, state.video, state.overlay}) == Result::InvalidView);
    assert(QueueCustomDisplayTargets(spec, limits, 1, &queue, &Request,
        {state.width, state.height, state.height, state.video, state.overlay}) == Result::InvalidView);
    assert(QueueCustomDisplayTargets(spec, limits, 1, &queue, &Request,
        {state.width, state.height, state.scale, state.video, state.video}) == Result::InvalidView);
    assert(QueueCustomDisplayTargets(spec, {}, 1, &queue, &Request, state.view()) == Result::InvalidPlan);
    assert(QueueCustomDisplayTargets({}, limits, 1, &queue, &Request, state.view()) == Result::InvalidPlan);
    assert(QueueCustomDisplayTargets({17,1,480,272}, limits, 1, &queue, &Request, state.view()) == Result::InvalidPlan);
    assert(QueueCustomDisplayTargets({2,1,4097,272}, limits, 1, &queue, &Request, state.view()) == Result::InvalidPlan);
    auto tiny = limits; tiny.maxTargetBytes = 1;
    assert(QueueCustomDisplayTargets(spec, tiny, 3, &queue, &Request, state.view()) == Result::InvalidPlan);
    tiny.maxTargetBytes = uint64_t(480) * 272 * 2 * 4;
    assert(QueueCustomDisplayTargets(spec, tiny, 1, &queue, &Request, state.view()) == Result::InvalidPlan);
    assert(Same(state, initial) && queue.events.empty() && queue.objects.empty());
    // A stray manager pointer is also a nonempty handle, not a fresh target.
    for (auto* handle : {&state.video, &state.overlay}) {
        for (bool object : {false, true}) {
            if (object) handle->object = &queue; else handle->manager = &queue;
            const auto before = state;
            assert(run(1) == Result::LiveResources && Same(state, before));
            assert(queue.events.empty());
            *handle = {};
        }
    }
    // Quality recreation, including before the previous deferred events drain.
    for (int32_t quality : {1, 2, 3, -1, 1}) {
        const size_t start = queue.events.size();
        assert(run(quality) == Result::Queued);
        assert(state.width == 480 && state.height == 272);
        assert(state.scale == CustomDisplaySupersamplingForQuality(quality));
        assert(queue.events.size() == start + 2);
        assert(queue.events[start].handle == &state.video);
        assert(queue.events[start+1].handle == &state.overlay);
        assert(state.video.object != state.overlay.object);
        assert(!static_cast<Object*>(state.video.object)->ready);
        const auto before = state;
        assert(run(quality) == Result::LiveResources && Same(state, before));
        assert(queue.events.size() == start + 2);
        Release(queue);
        assert(!state.video.object && !state.video.manager && !state.overlay.object && !state.overlay.manager);
        const auto released = queue.events.size();
        Release(queue); // already released: no duplicate destruction
        assert(queue.events.size() == released);
    }
    Drain(queue);
    for (const auto& object : queue.objects) assert(object->ready && object->destroyed);
    // Independent components/queues and physical dimensions: no shared mutable
    // definition, registry or global resolution is involved in this adapter.
    State other;
    Queue second{&other, {}, {}};
    assert(QueueCustomDisplayTargets({1,3,96,64}, limits, 3, &second, &Request, other.view()) == Result::Queued);
    assert(other.width == 96 && other.height == 64 && other.scale == 4);
    assert(state.width == 480 && state.height == 272);
    Drain(second);
    assert(static_cast<Object*>(other.video.object)->ready);
    Release(second); Drain(second);
    for (const auto& object : second.objects) assert(object->destroyed);
    puts("PASS: native target queue adapter, independent pixels, quality mapping, atomic rejection, deferred recreation and per-monitor isolation (test doubles only)");
}
