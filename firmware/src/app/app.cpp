#include "app/app.hpp"

namespace h0 {

bool App::faceSuits(FaceId f) const {
    if (f == FaceId::Hourglass) {
        // Meaningless without a duration to be a fraction of, and not worth
        // watching beyond the point where the drain stops being perceptible.
        return timer_.duration() > 0 && timer_.duration() <= kHourglassMaxUs;
    }
    return true;
}

FaceId App::face() const {
    // Fall back rather than draw a face that cannot represent the timer. The
    // alternative -- an hourglass frozen at full because the duration is an
    // hour -- looks like a bug.
    return faceSuits(face_) ? face_ : FaceId::Digits;
}

void App::cycleFace() {
    const FaceId order[3] = {FaceId::Hourglass, FaceId::Digits, FaceId::SplitFlap};
    int idx = 0;
    for (int i = 0; i < 3; ++i) if (order[i] == face_) idx = i;
    for (int step = 1; step <= 3; ++step) {
        const FaceId next = order[(idx + step) % 3];
        if (faceSuits(next)) { face_ = next; return; }
    }
}

bool App::setDuration(uint64_t us, uint64_t now) {
    // The dial is live only in the setting posture. Without this a pocket could
    // rewrite a running timer, and the gesture vocabulary has no undo.
    if (!flat_) return false;
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
            if (timer_.duration() == 0) return Feedback::Rejected; // nothing dialled in
            switch (timer_.state()) {
                case TimerModel::State::Paused:
                    timer_.resume(now);
                    return Feedback::Resumed;
                case TimerModel::State::Idle:
                    timer_.start(now);
                    return Feedback::Started;
                case TimerModel::State::Expired:
                    // Standing up a finished timer does not restart it. Restart
                    // is the flip, and it should stay the only thing that is.
                    return Feedback::Rejected;
                case TimerModel::State::Running:
                    return Feedback::None;
            }
            return Feedback::None;
        }

        case MotionEvent::Flip: {
            flat_ = false;
            if (timer_.duration() == 0) return Feedback::Rejected;
            timer_.reset(now);
            alarmOn_ = false;
            return Feedback::Reset;
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

    if (alarmOn_ && now - alarmSince_ >= kAlarmTimeoutUs) {
        // Give up making noise, but keep the expiry: the face still reads DONE
        // so the user learns what happened when they come back.
        alarmOn_ = false;
        return Feedback::AlarmOff;
    }

    return Feedback::None;
}

} // namespace h0
