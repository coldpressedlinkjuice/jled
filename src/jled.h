// Copyright (c) 2017 Jan Delgado <jdelgado[at]gmx.net>
// https://github.com/jandelgado/jled
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//
#ifndef SRC_JLED_H_
#define SRC_JLED_H_

#include <Arduino.h>

// Non-blocking LED abstraction class.
//
// Example Arduino sketch:
//   JLed led = JLed(LED_BUILTIN).Blink(500, 500).Repeat(10).DelayBefore(1000);
//
//   void setup() {}
//
//   void loop() {
//     led.Update();
//   }
//

template <typename T>
class TJLed {
 public:
    // a function f(t,period,param) that calculates the LEDs brightness for a
    // given point in time and the given period. param is an optionally user
    // provided parameter. t will always be in range [0..period-1].
    // f(period-1,period,param) will be called last to calculate the final
    // state of the LED.
    using BrightnessEvalFunction = uint8_t (*)(uint32_t t, uint16_t period,
                                               uintptr_t param);

    TJLed() = delete;
    explicit TJLed(const T& port) noexcept : port_(port) {}
    explicit TJLed(uint8_t led_pin) noexcept : TJLed(T(led_pin)) {}

    // update brightness of LED using the given brightness function
    //  (brightness)                     _________________
    // on 255 |                       ¸-'
    //        |                    ¸-'
    //        |                 ¸-'
    // off 0  |______________¸-'
    //        |<delay before>|<--period-->|<-delay after-> (time)
    //                       | func(t)    |
    //                       |<- num_repetitions times  ->
    bool Update() {
        if (!brightness_func_) {
            return false;
        }
        const auto now = millis();

        // no need to process updates twice during one time tick.
        if (last_update_time_ == now) {
            return true;
        }

        // last_update_time_ will be 0 on initialization, so this fails on
        // first call to this method.
        if (last_update_time_ == kTimeUndef) {
            last_update_time_ = now;
            time_start_ = now + delay_before_;
        }
        const auto delta_time = now - last_update_time_;
        last_update_time_ = now;
        // wait until delay_before time is elapsed before actually doing
        // anything
        if (delay_before_ > 0) {
            delay_before_ =
                max(static_cast<int64_t>(0),  // NOLINT
                    static_cast<int64_t>(delay_before_) - delta_time);
            if (delay_before_ > 0) return true;
        }

        if (!IsForever()) {
            const auto time_end =
                time_start_ +
                (uint32_t)(period_ + delay_after_) * num_repetitions_;

            if (now >= time_end) {
                // make sure final value of t=period-1 is set
                AnalogWrite(EvalBrightness(period_ - 1));
                brightness_func_ = nullptr;
                return false;
            }
        }

        // t cycles in range [0..period+delay_after-1]
        const auto t = (now - time_start_) % (period_ + delay_after_);

        if (t < period_) {
            SetInDelayAfterPhase(false);
            AnalogWrite(EvalBrightness(t));
        } else {
            if (!IsInDelayAfterPhase()) {
                // when in delay after phase, just call AnalogWrite()
                // once at the beginning.
                SetInDelayAfterPhase(true);
                AnalogWrite(EvalBrightness(period_ - 1));
            }
        }
        return true;
    }

    // turn LED on, respecting delay_before
    TJLed<T>& On() {
        period_ = 1;
        return Init(&TJLed::OnFunc);
    }

    // turn LED off, respecting delay_before
    TJLed<T>& Off() {
        period_ = 1;
        return Init(&TJLed::OffFunc);
    }

    // turn LED on or off, calls On() / Off()
    TJLed<T>& Set(bool on) { return on ? On() : Off(); }

    // Fade LED on
    TJLed<T>& FadeOn(uint16_t duration) {
        period_ = duration;
        return Init(&TJLed::FadeOnFunc);
    }

    // Fade LED off - acutally is just inverted version of FadeOn()
    TJLed<T>& FadeOff(uint16_t duration) {
        period_ = duration;
        return Init(&TJLed::FadeOffFunc);
    }

    // Set effect to Breathe, with the given period time in ms.
    TJLed<T>& Breathe(uint16_t period) {
        period_ = period;
        return Init(&TJLed::BreatheFunc);
    }

    // Set effect to Blink, with the given on- and off- duration values.
    TJLed<T>& Blink(uint16_t duration_on, uint16_t duration_off) {
        period_ = duration_on + duration_off;
        effect_param_ = duration_on;
        return Init(&TJLed::BlinkFunc);
    }

    // Use user provided function func as brightness function.
    TJLed<T>& UserFunc(BrightnessEvalFunction func, uint16_t period,
                       uintptr_t user_param = 0) {
        effect_param_ = user_param;
        period_ = period;
        return Init(func);
    }

    // set number of repetitions for effect.
    TJLed<T>& Repeat(uint16_t num_repetitions) {
        num_repetitions_ = num_repetitions;
        return *this;
    }

    // repeat Forever
    TJLed<T>& Forever() { return Repeat(kRepeatForever); }
    bool IsForever() const { return num_repetitions_ == kRepeatForever; }

    // Set amount of time to initially wait before effect starts. Time is
    // relative to first call of Update() method and specified in ms.
    TJLed<T>& DelayBefore(uint16_t delay_before) {
        delay_before_ = delay_before;
        return *this;
    }

    // Set amount of time to wait in ms after each iteration.
    TJLed<T>& DelayAfter(uint16_t delay_after) {
        delay_after_ = delay_after;
        return *this;
    }

    // Invert effect. If set, every effect calculation will be inverted, i.e.
    // instead of a, 255-a will be used.
    TJLed<T>& Invert() { return SetFlags(FL_INVERTED, true); }
    bool IsInverted() const { return GetFlag(FL_INVERTED); }

    // Set physical LED polarity to be low active. This inverts every signal
    // physically output to a pin.
    TJLed<T>& LowActive() { return SetFlags(FL_LOW_ACTIVE, true); }
    bool IsLowActive() const { return GetFlag(FL_LOW_ACTIVE); }

    // Stop current effect and turn LED immeadiately off
    void Stop() {
        // Immediately turn LED off and stop effect.
        brightness_func_ = nullptr;
        AnalogWrite(0);
    }

 protected:
    BrightnessEvalFunction brightness_func_ = nullptr;
    uintptr_t effect_param_ = 0;  // optional additional effect paramter.

    // internal control of the LED, does not affect
    // state and honors low_active_ flag
    void AnalogWrite(uint8_t val) {
        auto new_val = IsLowActive() ? kFullBrightness - val : val;
        port_.analogWrite(new_val);
    }

    TJLed<T>& Init(BrightnessEvalFunction func) {
        brightness_func_ = func;
        last_update_time_ = kTimeUndef;
        time_start_ = kTimeUndef;
        return *this;
    }

    TJLed<T>& SetFlags(uint8_t f, bool val) {
        if (val) {
            flags_ |= f;
        } else {
            flags_ &= ~f;
        }
        return *this;
    }
    bool GetFlag(uint8_t f) const { return (flags_ & f) != 0; }

    void SetInDelayAfterPhase(bool f) { SetFlags(FL_IN_DELAY_PHASE, f); }
    bool IsInDelayAfterPhase() const { return GetFlag(FL_IN_DELAY_PHASE); }

    uint8_t EvalBrightness(uint32_t t) const {
        const auto val = brightness_func_(t, period_, effect_param_);
        return IsInverted() ? kFullBrightness - val : val;
    }

    // permanently turn LED on
    static uint8_t OnFunc(uint32_t, uint16_t, uintptr_t) {
        return kFullBrightness;
    }

    // permanently turn LED off
    static uint8_t OffFunc(uint32_t, uint16_t, uintptr_t) {
        return kZeroBrightness;
    }

    // BlincFunc does one on-off cycle in the specified period. The effect_param
    // specifies the time the effect is on.
    static uint8_t BlinkFunc(uint32_t t, uint16_t period,
                             uintptr_t effect_param) {
        return (t < effect_param) ? kFullBrightness : kZeroBrightness;
    }

    // fade LED on
    // https://www.wolframalpha.com/input/?i=plot+(exp(sin((x-100%2F2.)*PI%2F100))-0.36787944)*108.0++x%3D0+to+100
    // The fade-on func is an approximation of
    //   y(x) = exp(sin((t-period/2.) * PI / period)) - 0.36787944) * 108.)
    static uint8_t FadeOnFunc(uint32_t t, uint16_t period, uintptr_t) {
        if (t + 1 >= period) return kFullBrightness;

        // approximate by linear interpolation.
        // scale t according to period to 0..255
        t = ((t << 8) / period) & 0xff;
        const auto i = (t >> 5);  // -> i will be in range 0 .. 7
        const auto y0 = kFadeOnTable[i];
        const auto y1 = kFadeOnTable[i + 1];
        const auto x0 = i << 5;  // *32

        // y(t) = mt+b, with m = dy/dx = (y1-y0)/32 = (y1-y0) >> 5
        return (((t - x0) * (y1 - y0)) >> 5) + y0;
    }

    // Fade LED off - inverse of FadeOnFunc()
    static uint8_t FadeOffFunc(uint32_t t, uint16_t period, uintptr_t) {
        return FadeOnFunc(period - t, period, 0);
    }

    // The breathe func is composed by fadein and fade-out with one each half
    // period.  we approximate the following function:
    //   y(x) = exp(sin((t-period/4.) * 2. * PI / period)) - 0.36787944) * 108.)
    // idea see: http://sean.voisen.org/blog/2011/10/breathing-led-with-arduino/
    // But we do it with integers only.
    static uint8_t BreatheFunc(uint32_t t, uint16_t period, uintptr_t) {
        if (t + 1 >= period) return kZeroBrightness;
        const uint16_t periodh = period >> 1;
        return t < periodh ? FadeOnFunc(t, periodh, 0)
                           : FadeOffFunc(t - periodh, periodh, 0);
    }

 private:
    // pre-calculated fade-on function. This table samples the function
    //   y(x) =  exp(sin((t - period / 2.) * PI / period)) - 0.36787944) * 108.
    // at x={0,32,...,256}. In FadeOnFunc() we us linear interpolation to
    // approximate the original function (so we do not need fp-ops).
    // fade-off and breath functions are all derived from fade-on, see below.
    // (To save some additional bytes, we could place it in PROGMEM sometime)
    static constexpr uint8_t kFadeOnTable[] = {0,   3,   13,  33, 68,
                                               118, 179, 232, 255};
    static constexpr uint16_t kRepeatForever = 65535;
    static constexpr uint32_t kTimeUndef = -1;
    static constexpr uint8_t FL_INVERTED = (1 << 0);
    static constexpr uint8_t FL_LOW_ACTIVE = (1 << 1);
    static constexpr uint8_t FL_IN_DELAY_PHASE = (1 << 2);
    static constexpr uint8_t kFullBrightness = 255;
    static constexpr uint8_t kZeroBrightness = 0;
    T port_;
    uint8_t flags_ = 0;

    uint16_t num_repetitions_ = 1;
    uint32_t last_update_time_ = kTimeUndef;
    uint16_t delay_before_ = 0;  // delay before the first effect starts
    uint16_t delay_after_ = 0;   // delay after each repetition
    uint32_t time_start_ = kTimeUndef;
    uint16_t period_ = 0;
};

template <typename T>
constexpr uint8_t TJLed<T>::kFadeOnTable[];

#ifdef ESP32
#include "esp32_analog_writer.h"  // NOLINT
using JLed = TJLed<Esp32AnalogWriter>;
template class TJLed<Esp32AnalogWriter>;
#elif ESP8266
#include "esp8266_analog_writer.h"  // NOLINT
using JLed = TJLed<Esp8266AnalogWriter>;
template class TJLed<Esp8266AnalogWriter>;
#else
#include "arduino_analog_writer.h"  // NOLINT
using JLed = TJLed<ArduinoAnalogWriter>;
template class TJLed<ArduinoAnalogWriter>;
#endif

#endif  // SRC_JLED_H_
