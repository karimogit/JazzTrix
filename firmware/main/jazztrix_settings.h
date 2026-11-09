#ifndef JAZZTRIX_SETTINGS_H
#define JAZZTRIX_SETTINGS_H
#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JAZZTRIX_RADIO_MAX 8
#define JAZZTRIX_NFC_LABEL_MAX 40
#define JAZZTRIX_METRIC_COUNT 5
#define JAZZTRIX_TOOLBAR_SLOTS 4 /* legacy order kept in place so old desk blobs still line up */
#define JAZZTRIX_BAR_SLOTS 6
#define JAZZTRIX_SETTINGS_MAGIC 0x4a415a5a /* JAZZ */

enum jazztrix_metric {
    JAZZ_MET_WIND = 0,
    JAZZ_MET_RAIN = 1,
    JAZZ_MET_PRESS = 2,
    JAZZ_MET_AIR = 3,
    JAZZ_MET_UV = 4,
};

enum jazztrix_toolbar {
    JAZZ_TB_PLACE = 0,
    JAZZ_TB_CLOCK = 1,
    JAZZ_TB_WIFI = 2,
    JAZZ_TB_BATTERY = 3,
    JAZZ_TB_DATE = 4,
    JAZZ_TB_WEEKDAY = 5,
};

enum jazztrix_block {
    JAZZ_BLK_TEMP = 1,
    JAZZ_BLK_FORECAST = 2,
    JAZZ_BLK_COFFEE = 4,
    JAZZ_BLK_RADIO = 8,
    JAZZ_BLK_HAIKU = 16,
};

/* Bottom row. 0 brew, 1 radio, 2 haiku. */
#define JAZZTRIX_FOOT_SLOTS 3

/* Outlines on the dashboard. Degrees and the sky line start off. The rest match the paper today. */
enum jazztrix_frame {
    JAZZ_FRAME_FORECAST = 1,
    JAZZ_FRAME_DEGREES = 2,
    JAZZ_FRAME_SKY = 4,
    JAZZ_FRAME_TILES = 8,
    JAZZ_FRAME_BREW = 16,
    JAZZ_FRAME_RADIO = 32,
    JAZZ_FRAME_HAIKU = 64,
};
#define JAZZ_FRAME_DEFAULT (JAZZ_FRAME_TILES | JAZZ_FRAME_BREW | JAZZ_FRAME_RADIO | JAZZ_FRAME_HAIKU)

/* NOTE4C pigments: 0 black, 1 paper, 2 yellow, 3 red */
typedef struct {
    uint32_t magic;
    uint16_t struct_size;
    char nfc_label[JAZZTRIX_NFC_LABEL_MAX];
    uint8_t brush;
    uint8_t radio_count;
    char radio_name[JAZZTRIX_RADIO_MAX][24];
    char radio_url[JAZZTRIX_RADIO_MAX][128];
    bool dark_mode;
    bool unit_suffix;
    uint8_t wind_unit; /* 0 m/s, 1 km/h, 2 mph */
    uint8_t theme_preset; /* 0 classic, 1 sunset, 2 ocean, 3 custom, 4 marquee, 5 chorus, 6 brass, 7 spotlight */
    uint8_t color_bar;
    uint8_t color_accent;
    uint8_t color_alert;
    uint8_t color_hi;
    uint8_t color_lo;
    uint8_t toolbar_order[JAZZTRIX_TOOLBAR_SLOTS];
    uint8_t toolbar_visible; /* bit i = slot id i */
    uint8_t metric_order[JAZZTRIX_METRIC_COUNT];
    uint8_t metric_visible; /* bit = metric id */
    uint8_t block_visible; /* jazztrix_block bits */
    bool upper_titles; /* box titles on the paper */
    uint8_t reserved_pad[4]; /* [0] paper: 0 automatic, 1 NOTE4, 2 NOTE4C */
    uint8_t bar_order[JAZZTRIX_BAR_SLOTS]; /* weekday, date, time, place, wifi, battery */
    uint8_t date_format; /* 0: 9 Nov, 1: Nov 9, 2: 09/11, 3: 11/09, 4: 2026-11-09 */
    uint8_t weekday_style; /* 0 short, 1 long */
    uint8_t reserved_grow[4];
    uint8_t forecast_hours; /* 3, 6, 9, or 12 hours ahead on the paper */
    uint8_t forecast_style; /* 0 graph, 1 bars */
    uint8_t reserved_forecast[2];
    uint8_t borders; /* jazztrix_frame bits */
    uint8_t reserved_borders[3];
    uint8_t haiku_pick; /* 0 a new one each day, 1–30 a chosen poem */
    uint8_t reserved_haiku[3];
    uint8_t foot_order[JAZZTRIX_FOOT_SLOTS]; /* brew, radio, haiku */
    uint8_t reserved_foot;
} jazztrix_settings_t;

typedef struct {
    uint8_t page_fill;
    uint8_t bar_ink;
    uint8_t bar_text;
    uint8_t clock;
    uint8_t body;
    uint8_t frame;
    uint8_t temp_main;
    uint8_t temp_hi;
    uint8_t temp_lo;
    uint8_t accent;
    uint8_t alert;
} jazztrix_ink_t;

void jazztrix_settings_init(void);
void jazztrix_settings_get(jazztrix_settings_t *out);
esp_err_t jazztrix_settings_save(const jazztrix_settings_t *in);
void jazztrix_settings_apply_config(void);
void jazztrix_inks(const jazztrix_settings_t *s, jazztrix_ink_t *out);
bool jazztrix_block_on(const jazztrix_settings_t *s, enum jazztrix_block b);
bool jazztrix_metric_on(const jazztrix_settings_t *s, enum jazztrix_metric m);
const char *jazztrix_nfc_label(void);
/* 0 automatic, 1 NOTE4 (black and white), 2 NOTE4C (four pigments). */
uint8_t jazztrix_paper_mode(void);
int jazztrix_radio_count(void);
const char *jazztrix_radio_name(int i);
const char *jazztrix_radio_url(int i);
double jazztrix_wind_display(double ms, uint8_t unit);
const char *jazztrix_wind_unit_suffix(uint8_t unit);
void jazztrix_metric_title(char *out, size_t len, enum jazztrix_metric m,
                           const jazztrix_settings_t *s);
void jazztrix_upper_box_title(char *s, bool upper);
bool jazztrix_settings_import_playlist(const char *text, jazztrix_settings_t *out);
cJSON *jazztrix_settings_json(void);
bool jazztrix_settings_decode(const char *json, size_t len, jazztrix_settings_t *out, char *why,
                             size_t why_len);

#endif
