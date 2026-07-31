#include "board/power_button.hpp"

#include "hardware/gpio.h"
#include "pico/stdlib.h"

#include "board/buzzer.hpp"
#include "board/cst816.hpp"
#include "board/pins.hpp"
#include "board/qmi8658.hpp"
#include "board/st7789_1in69.hpp"

namespace board {

void PowerButton::begin() {
    gpio_init(board::power::PWR_KEY);
    gpio_set_dir(board::power::PWR_KEY, GPIO_IN);
    gpio_pull_up(board::power::PWR_KEY); // reads LOW while pressed
    state_ = false;
    last_ = false;
}

bool PowerButton::isDown() {
    const bool raw = !gpio_get(board::power::PWR_KEY);
    if (raw == last_) state_ = raw; // two agreeing samples to change
    last_ = raw;
    return state_;
}

void PowerButton::shutdown(St7789_1in69& lcd, Cst816& touch, Qmi8658& imu,
                           Buzzer& buzzer) {
    // Precondition: the button must already be released. See the header --
    // holding it down leaves Q3's gate pulled low through D1, independently of
    // GPIO15, so dropping the latch here would appear to do nothing and the
    // board would die later, at release, apparently at random. The only caller
    // (main.cpp, task 4) only reaches this on h0::PowerAction::PowerOff, which
    // h0::PowerPolicy emits solely from the release sample, so that is already
    // guaranteed by the time this runs.
    //
    // Order below is load-bearing (design spec section 5). There is NO RTC step
    // here: the PCF85063A has no driver in this tree, so power-and-time.md
    // section 6.3's alarm disarm and session-cookie clear have nothing to act
    // on -- pins.hpp:55's ADDR_RTC is the only trace of the part in this repo.
    buzzer.stop();

    // Backlight first, THEN SLPIN. SLPIN blanks the panel itself, so the
    // reverse order shows a lit blank screen. This is a blind write -- there is
    // no MISO, so the only confirmation is a meter.
    lcd.setBacklight(0);
    lcd.waitIdle();
    lcd.sleepIn();

    touch.setHeldAwake(false);
    imu.powerDown();

    // The latch. R4 pulls T1's base down, Q3 opens, the rail collapses in
    // ~50 us. This is the only place outside latchPower() at main() line 1
    // that may write GPIO15, and the two can never race: latchPower() runs
    // once at boot before anything else, and this runs only after the button
    // has been released, which is at minimum a two-second hold after boot.
    gpio_put(board::power::SYS_EN, 0);

    // On battery this is never reached. On USB, D4 keeps VSYS alive and the
    // caller gets control back -- which is why every caller shows a message
    // rather than assuming this is the end.
    sleep_ms(50);
}

} // namespace board
