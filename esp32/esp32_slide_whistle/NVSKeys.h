/*
 * NVSKeys.h — Clés NVS centralisées
 *
 * Avant ce fichier, les clés NVS étaient des littéraux ("ch", "nMin"…)
 * répartis dans WebInterface.h. Ce fichier les expose comme constantes
 * nommées pour :
 *   - éviter les fautes de frappe silencieuses (ex. "pwmFul" vs "pwmFull")
 *   - faciliter la migration de schéma (renommer un seul const)
 *   - documenter le mapping clé ↔ champ
 *
 * Limite ESP32 NVS : nom de clé ≤ 15 caractères (16 avec le \0).
 * Toutes les clés ci-dessous respectent cette borne.
 */

#ifndef NVS_KEYS_H
#define NVS_KEYS_H

namespace NVSKeys {
  // ---- Namespace "fluteN" -------------------------------------------------
  constexpr const char* MIDI_CHANNEL    = "ch";
  constexpr const char* NOTE_MIN        = "nMin";
  constexpr const char* NOTE_MAX        = "nMax";
  constexpr const char* ENABLED         = "enabled";
  constexpr const char* MUTED           = "muted";
  constexpr const char* SPEED           = "speed";
  constexpr const char* ACCEL           = "accel";
  constexpr const char* PWM_FULL        = "pwmFull";
  constexpr const char* PWM_HOLD        = "pwmHold";
  constexpr const char* WAIT_DELAY      = "wait";
  constexpr const char* LEGATO          = "legato";
  constexpr const char* VEL_CURVE       = "velCurve";
  constexpr const char* CUSTOM_NAME     = "cName";
  constexpr const char* CC_BREATH       = "ccBreath";
  constexpr const char* CC_EXPR         = "ccExpr";
  constexpr const char* CC_VOLUME       = "ccVolume";
  constexpr const char* CC_VIBRATO      = "ccVibrato";
  constexpr const char* CC_SUSTAIN      = "ccSustain";
  constexpr const char* TRANSPOSE       = "trans";
  constexpr const char* USE_LUT         = "useLUT";
  constexpr const char* LUT_BLOB        = "lut";

  // ---- Namespace "pressure" ----------------------------------------------
  constexpr const char* P_TARGET        = "target";
  constexpr const char* P_MIN           = "min";
  constexpr const char* P_MAX           = "max";
  constexpr const char* P_SAFETY        = "safety";
  constexpr const char* P_FILL_MS       = "fillMs";

  // ---- Namespace "wifi" --------------------------------------------------
  constexpr const char* WIFI_SSID       = "ssid";
  constexpr const char* WIFI_PASSWORD   = "password";

  // ---- Namespace "presets_idx" -------------------------------------------
  constexpr const char* PRESETS_LIST    = "list";

  // Helper : nom de namespace par flûte. Limite NVS = 15 chars max.
  // "flute" + uint8_t (max 3 chars) = 8 chars → OK.
  inline String fluteNamespace(uint8_t id) {
    char buf[16];
    snprintf(buf, sizeof(buf), "flute%u", id);
    return String(buf);
  }
}

#endif // NVS_KEYS_H
