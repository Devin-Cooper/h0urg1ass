#include "board/power_button.hpp"

#include "hardware/gpio.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/watchdog.h"
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

    // MUST come before the latch drop, and it is what stops a power-off being
    // undone eight seconds later.
    //
    // Everything below assumes the rail collapses. When it does not -- VBUS is
    // present and D4 is holding VSYS up, which the onUsb guard misses for a
    // charger or a power-only cable, since it reads USB ENUMERATION and not
    // VBUS -- execution survives into the spin at the bottom of this function.
    // That spin feeds nothing, so main()'s 8 s watchdog bites, and a watchdog
    // reset is precisely the case latchPower() was taught to survive: the pad
    // ISO bit holds GPIO15 through it, main() re-asserts the latch, and the
    // device that was just switched off is on again. Then it idles out and
    // does the whole thing again. The two features are each correct; together
    // they make power-off reversible.
    //
    // Disabling rather than feeding it, because there is nothing left to
    // protect: no main loop, panel in SLPIN, IMU down. Both would equally stop
    // the reboot, and this one says what it means.
    //
    // It does not cost the reflash route the watchdog exists for. That route
    // needs firmware alive enough to answer a 1200-baud BOOTSEL handshake, and
    // the SDK services `tud_task()` from a low-priority IRQ rather than from
    // the main loop -- so USB keeps running through the spin below whatever
    // this core is doing. The watchdog's actual quarry is a clock-gating
    // mistake that parks the core in WFE with USB's clocks gated too, which is
    // a different failure and is still covered: this only disarms it once the
    // device has decided to be off.
    watchdog_disable();

    // The latch. R4 pulls T1's base down, Q3 opens, the rail collapses in
    // ~50 us. This is the only place outside latchPower() at main() line 1
    // that may write GPIO15, and the two can never race: latchPower() runs
    // once at boot before anything else, and this runs only after the button
    // has been released, which is at minimum a two-second hold after boot.
    //
    // The SIO write ALONE DOES NOTHING, and that was the actual bug behind the
    // reboot this function's watchdog_disable() was first blamed for.
    // latchPower() finishes by SETTING this pad's ISO bit, which is what makes
    // the latch survive a watchdog reset -- and pad isolation works by cutting
    // the pad off from SIO and holding its level. So the store below lands in
    // the SIO register and the pad stays high, on battery as much as on USB.
    // The latch never opened; the rail never collapsed; execution always ran on
    // into the spin at the bottom of this function.
    //
    // ISO resets to 1 and the SDK CLEARS it in gpio_set_function() to make a
    // pad usable at all ("Remove this once the pad is configured by software").
    // Clearing it here hands GPIO15 back to SIO, which is already holding the 0
    // written above, so the pad makes one clean transition to low. R4 then
    // pulls T1's base down, Q3 opens, and the rail collapses in ~50 us.
    //
    // The symptom that identified this: with the watchdog gone the device
    // looked off but was not -- and pressing RESET (Key3, wired to RUN) and
    // then power brought it up. RUN resets the always-on domain, which is the
    // one case the isolation does not survive; the pad was released, THEN the
    // rail dropped, and only then could the button start the board. A reset
    // button cannot act on a board that has no power, so the rail being alive
    // was never in doubt once that was reported.
    gpio_put(board::power::SYS_EN, 0);
    hw_clear_bits(&pads_bank0_hw->io[board::power::SYS_EN], PADS_BANK0_GPIO0_ISO_BITS);

    // On battery the rail collapses about here and none of this is reached.
    // On USB, D4 keeps VSYS alive regardless of GPIO15, so execution
    // survives -- and a device that has just decided to be off must stay
    // off, not fall back into a live main loop with the panel in SLPIN, the
    // IMU powered down and touch asleep: nothing left running would ever
    // re-initialise them, control would be running blind, and the idle route
    // would see the same idle conditions and call this function again next
    // frame, forever. Spin here instead, so "off" is honest until the cable
    // comes out. Design spec section 5 step 8 and docs/power-and-time.md
    // section 6.3 both specify this loop.
    //
    // "Until the cable comes out" is only true because of the watchdog_disable()
    // above. Without it this loop is not a resting place at all -- it is an 8 s
    // fuse on a reboot, and the reboot re-latches power. Do not add anything
    // here that re-arms the watchdog.
    sleep_ms(50);
    while (true) tight_loop_contents();
}

} // namespace board
