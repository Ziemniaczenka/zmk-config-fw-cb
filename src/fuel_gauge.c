/*
 * Copyright (c) 2026 Shaedil
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT shaedil_npm1300_fuel_gauge

#include <math.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include <nrf_fuel_gauge.h>

LOG_MODULE_REGISTER(fuel_gauge, CONFIG_SENSOR_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1);

#define CHG_COMPLETE BIT(1)
#define CHG_TRICKLE BIT(2)
#define CHG_CC BIT(3)
#define CHG_CV BIT(4)

#define VBUS_PRESENT BIT(0)
#define VBUS_LIMITED BIT(1)
#define VBUS_SUSPENDED BIT(4)

#define BUSY_INTERVAL K_SECONDS(1)
#define IDLE_INTERVAL K_SECONDS(10)
#define BUSY_CURRENT_A 0.02f
#define SAVE_INTERVAL_MS (60 * 60 * 1000)
// A freshly connected cell reads wrong for a moment (contact bounce, charger
// detection), so it must stay connected this long before the gauge starts
#define SETTLE_MS (3 * 1000)
#define START_STEP_S 0.001f
// Saved level vs the level from the voltage, in percentage points
#define SAVED_STATE_MAX_DIFF 20.0f
#define STATUS_INTERVAL_MS (60 * 1000)
// Below the charger's dead-battery level; an unplugged cell reads ~0 V
#define NO_BATTERY_V 1.0f
#define STACK_SIZE 3072

struct sample {
    float v;
    float i;
    float t;
    int32_t chg;
    int32_t vbus;
};

static const struct battery_model model = {
#include "battery_model.inc"
};

static const struct device *const charger = DEVICE_DT_GET(DT_INST_PHANDLE(0, charger));

static atomic_t soc = ATOMIC_INIT(-1);

static uint8_t state[1024];
static size_t state_len;
static K_SEM_DEFINE(settings_loaded, 0, 1);

static float get(enum sensor_channel chan) {
    struct sensor_value val;

    sensor_channel_get(charger, chan, &val);
    return sensor_value_to_float(&val);
}

static int measure(struct sample *s) {
    struct sensor_value val;
    int err = sensor_sample_fetch(charger);

    if (err) {
        return err;
    }

    s->v = get(SENSOR_CHAN_GAUGE_VOLTAGE);
    s->t = get(SENSOR_CHAN_GAUGE_TEMP);
    // nRF Fuel Gauge counts discharge as positive
    s->i = -get(SENSOR_CHAN_GAUGE_AVG_CURRENT);

    sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &val);
    s->chg = val.val1;
    sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS, &val);
    s->vbus = val.val1;

    return 0;
}

static enum nrf_fuel_gauge_charge_state charge_state(const struct sample *s) {
    if (s->chg & CHG_COMPLETE) {
        return NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
    }
    if (s->chg & CHG_TRICKLE) {
        return NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
    }
    if (s->chg & CHG_CC) {
        return (s->vbus & VBUS_LIMITED) ? NRF_FUEL_GAUGE_CHARGE_STATE_CC_LIMITED
                                        : NRF_FUEL_GAUGE_CHARGE_STATE_CC;
    }
    if (s->chg & CHG_CV) {
        return NRF_FUEL_GAUGE_CHARGE_STATE_CV;
    }
    return NRF_FUEL_GAUGE_CHARGE_STATE_IDLE;
}

// VBUS in suspend mode supplies nothing; the board runs from the cell
static bool usb_powered(const struct sample *s) {
    return (s->vbus & (VBUS_PRESENT | VBUS_SUSPENDED)) == VBUS_PRESENT;
}

static void report_vbus(const struct sample *s) {
    nrf_fuel_gauge_ext_state_update(usb_powered(s)
                                        ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
                                        : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED,
                                    NULL);
}

static void report_charge_state(const struct sample *s) {
    nrf_fuel_gauge_ext_state_update(
        NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_STATE_CHANGE,
        &(union nrf_fuel_gauge_ext_state_info_data){.charge_state = charge_state(s)});
}

// BATTERYDETECTED is a reserved bit in the nPM1300 datasheet: it stays set
// after the cell is unplugged and clears in USB suspend with the cell still
// there. An unplugged cell reads ~0 V, and without VBUS the board only runs
// from a cell, so the voltage alone decides.
static bool battery_present(const struct sample *s) {
    return s->v >= NO_BATTERY_V;
}

static int init_level(const struct nrf_fuel_gauge_init_parameters *params,
                      const struct sample *s, float *pct) {
    int err = nrf_fuel_gauge_init(params, NULL);

    if (err) {
        return err;
    }
    // A minimal step only to read back the starting level
    return nrf_fuel_gauge_process(s->v, s->i, s->t, START_STEP_S, pct, NULL);
}

// The saved state belongs to the cell that was there before the reset, so
// it is only offered for that one. It is still dropped when it disagrees
// with the voltage: it may be up to an hour old, or from a bad start.
static int start(const struct sample *s, bool use_saved_state) {
    int err;
    float pct;
    float resumed = -1.0f;
    struct nrf_fuel_gauge_init_parameters params =
        NRF_FUEL_GAUGE_DEFAULT_INIT_PARAMETERS_SECONDARY(s->v, s->i, s->t, &model);

    err = init_level(&params, s, &pct);
    if (err) {
        return err;
    }

    if (use_saved_state && state_len == nrf_fuel_gauge_state_size) {
        params.state = state;
        params.state_size = state_len;

        if (init_level(&params, s, &resumed) == 0 &&
            fabsf(resumed - pct) <= SAVED_STATE_MAX_DIFF) {
            pct = resumed;
        } else {
            LOG_INF("Saved state (%d%%) disagrees with the voltage (%d%%), using the voltage",
                    (int)lroundf(resumed), (int)lroundf(pct));
            params.state = NULL;
            params.state_size = 0;
            err = init_level(&params, s, &pct);
            if (err) {
                return err;
            }
        }
    }

    atomic_set(&soc, lroundf(CLAMP(pct, 0.0f, 100.0f)));

    // The model's capacity is not published; the library derives its
    // minimum fresh capacity from it (95 % by default)
    struct nrf_fuel_gauge_config_parameters cfg;

    if (nrf_fuel_gauge_config_params_current_get(&cfg) == 0) {
        LOG_INF("Battery model \"%s\", minimum capacity %d mAh", model.name,
                isnan(cfg.battery_min_capacity) ? -1 : (int)lroundf(cfg.battery_min_capacity));
    }

    float limit = get(SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT);

    nrf_fuel_gauge_ext_state_update(
        NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_CURRENT_LIMIT,
        &(union nrf_fuel_gauge_ext_state_info_data){.charge_current_limit = limit});
    nrf_fuel_gauge_ext_state_update(
        NRF_FUEL_GAUGE_EXT_STATE_INFO_TERM_CURRENT,
        &(union nrf_fuel_gauge_ext_state_info_data){.charge_term_current = limit / 10.0f});
    report_vbus(s);
    report_charge_state(s);

    return 0;
}

static void save(void) {
    if (nrf_fuel_gauge_state_get(state, sizeof(state)) == 0) {
        settings_save_one("fuel_gauge/state", state, nrf_fuel_gauge_state_size);
    }
}

// Logged on every charger or VBUS status change and once a minute, so the
// readings can be checked whenever a log terminal is attached
static void log_status(const struct sample *s) {
    static struct sample shown;
    static int64_t shown_at;
    int64_t now = k_uptime_get();

    if (shown_at != 0 && s->chg == shown.chg && s->vbus == shown.vbus &&
        now - shown_at < STATUS_INTERVAL_MS) {
        return;
    }

    LOG_INF("%d mV, %+d mA (+ charging), %d C, charger 0x%02x, VBUS 0x%02x, level %d%%",
            (int)lroundf(s->v * 1000.0f), (int)lroundf(-s->i * 1000.0f), (int)lroundf(s->t),
            s->chg, s->vbus, (int)atomic_get(&soc));
    shown = *s;
    shown_at = now;
}

static void run(void) {
    struct sample prev;
    struct sample s;
    bool running = false;
    // Until a disconnect is seen, the cell is the one from before the reset
    bool cell_from_boot = true;
    int64_t present_since = -1;
    int64_t last = 0;
    int64_t saved = 0;
    int err;

    if (!device_is_ready(charger)) {
        LOG_ERR("Charger not ready");
        return;
    }

    if (IS_ENABLED(CONFIG_SETTINGS)) {
        k_sem_take(&settings_loaded, K_SECONDS(10));
    }

    for (;;) {
        if (measure(&s)) {
            k_sleep(IDLE_INTERVAL);
            continue;
        }

        log_status(&s);

        if (!battery_present(&s)) {
            if (running) {
                LOG_INF("Battery disconnected");
                running = false;
            }
            // Any cell connected from now on may be a different one
            cell_from_boot = false;
            present_since = -1;
            // Unknown rather than a level of no cell; nothing is saved
            atomic_set(&soc, -1);
            k_sleep(BUSY_INTERVAL);
            continue;
        }

        if (!running) {
            if (present_since < 0) {
                present_since = k_uptime_get();
            }
            if (k_uptime_get() - present_since < SETTLE_MS) {
                k_sleep(BUSY_INTERVAL);
                continue;
            }

            err = start(&s, cell_from_boot);
            if (err) {
                LOG_DBG("Failed to start the fuel gauge (%d)", err);
                k_sleep(IDLE_INTERVAL);
                continue;
            }

            LOG_INF("Battery connected, %d mV, level %d%%", (int)(s.v * 1000.0f),
                    (int)atomic_get(&soc));
            running = true;
            last = k_uptime_get();
            saved = last;
            prev = s;
            k_sleep(BUSY_INTERVAL);
            continue;
        }

        if (usb_powered(&s) != usb_powered(&prev)) {
            report_vbus(&s);
        }
        if (s.chg != prev.chg || ((s.vbus ^ prev.vbus) & VBUS_LIMITED)) {
            report_charge_state(&s);
        }

        float pct;
        float dt = (float)k_uptime_delta(&last) / 1000.0f;

        if (nrf_fuel_gauge_process(s.v, s.i, s.t, dt, &pct, NULL) == 0) {
            atomic_set(&soc, lroundf(CLAMP(pct, 0.0f, 100.0f)));
        }

        if (IS_ENABLED(CONFIG_SETTINGS) && last - saved >= SAVE_INTERVAL_MS) {
            save();
            saved = last;
        }

        prev = s;

        bool busy = usb_powered(&s) || fabsf(s.i) >= BUSY_CURRENT_A;

        k_sleep(busy ? BUSY_INTERVAL : IDLE_INTERVAL);
    }
}

K_THREAD_DEFINE(fuel_gauge_thread, STACK_SIZE, run, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

#if IS_ENABLED(CONFIG_SETTINGS)
static int state_set(const char *name, size_t len, settings_read_cb read, void *arg) {
    if (!settings_name_steq(name, "state", NULL) || len > sizeof(state)) {
        return -ENOENT;
    }

    ssize_t n = read(arg, state, len);

    state_len = n > 0 ? n : 0;
    return 0;
}

static int state_commit(void) {
    k_sem_give(&settings_loaded);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(fuel_gauge, "fuel_gauge", NULL, state_set, state_commit, NULL);
#endif

static int fuel_gauge_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    return atomic_get(&soc) < 0 ? -EAGAIN : 0;
}

static int fuel_gauge_channel_get(const struct device *dev, enum sensor_channel chan,
                                  struct sensor_value *val) {
    int pct = atomic_get(&soc);

    if (chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) {
        return -ENOTSUP;
    }
    if (pct < 0) {
        return -EAGAIN;
    }

    val->val1 = pct;
    val->val2 = 0;
    return 0;
}

static DEVICE_API(sensor, fuel_gauge_api) = {
    .sample_fetch = fuel_gauge_sample_fetch,
    .channel_get = fuel_gauge_channel_get,
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,
                      &fuel_gauge_api);
