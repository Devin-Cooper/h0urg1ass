#include "app/app.hpp"

namespace h0 {

bool App::setDuration(uint64_t us, uint64_t now) {
    // The dial is live only where the picker is. Without this a pocket could
    // rewrite a running timer, and the gesture vocabulary has no undo.
    if (!settingPosture()) return false;
    (void)now;
    timer_.setDuration(us);
    alarmOn_ = false;
    return true;
}

Feedback App::onMotion(MotionEvent e, uint64_t now) {
    switch (e) {
        case MotionEvent::Settled: {
            // Laid flat: pause, and enter the setting posture.
            flat_ = true;
            const bool wasAlarming = alarmOn_;
            alarmOn_ = false;
            if (timer_.isExpired()) {
                // Flat is where a new time gets dialled, so clear the finished
                // run rather than leaving DONE on screen under the dial.
                timer_.acknowledge();
                return wasAlarming ? Feedback::AlarmOff : Feedback::None;
            }
            if (timer_.isRunning()) {
                timer_.pause(now);
                return Feedback::Paused;
            }
            return Feedback::None;
        }

        case MotionEvent::Raised: {
            flat_ = false;
            if (timer_.state() == TimerModel::State::Paused) {
                timer_.resume(now);
                return Feedback::Resumed;
            }
            // Standing it up is no longer a start -- upright is where you SET
            // it, and the flip is the only way to begin. Silent rather than
            // Rejected: this is an ordinary way to hold the device, and a
            // posture the user adopts constantly must not buzz at them.
            return Feedback::None;
        }

        case MotionEvent::Flip: {
            flat_ = false;
            if (timer_.duration() == 0) return Feedback::Rejected;
            // Reset is the most emphatic pattern in the buzzer's vocabulary
            // because it is the only destructive action. A flip from idle
            // destroys nothing, so it must not claim to.
            const bool wasIdle = (timer_.state() == TimerModel::State::Idle);
            timer_.reset(now);
            alarmOn_ = false;
            return wasIdle ? Feedback::Started : Feedback::Reset;
        }

        case MotionEvent::Tipped: {
            flat_ = false;
            // Only ever a pause. Flat and face down already own silencing an
            // alarm and acknowledging an expiry; giving a third posture a
            // fourth job adds edges for nothing.
            if (timer_.isRunning()) {
                timer_.pause(now);
                return Feedback::Paused;
            }
            return Feedback::None;
        }

        case MotionEvent::Silence: {
            flat_ = false;
            if (alarmOn_) {
                alarmOn_ = false;
                timer_.acknowledge();
                return Feedback::AlarmOff;
            }
            // Face down with nothing ringing is not a command. It is also how
            // the device ends up when put away, so it must not do anything.
            return Feedback::None;
        }

        case MotionEvent::None:
            return Feedback::None;
    }
    return Feedback::None;
}

Feedback App::tick(uint64_t now) {
    const bool wasExpired = timer_.isExpired();
    timer_.tick(now);

    if (!wasExpired && timer_.isExpired()) {
        alarmOn_ = true;
        alarmSince_ = now;
        return Feedback::AlarmOn;
    }

    if (alarmOn_ && now - alarmSince_ >= alarmTimeoutUs_) {
        // Give up making noise, but keep the expiry: the face still reads DONE
        // so the user learns what happened when they come back.
        alarmOn_ = false;
        return Feedback::AlarmOff;
    }

    return Feedback::None;
}

} // namespace h0
