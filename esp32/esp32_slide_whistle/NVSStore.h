/*
 * NVSStore.h — Persistance NVS pour Flute / PressureControl
 *
 * Centralise toutes les opérations save/load NVS, hors du WebInterface.
 * Utilise les clés définies dans NVSKeys.h (un seul endroit pour tout
 * renommer/migrer).
 *
 * Usage :
 *   Preferences prefs;
 *   NVSStore::saveFlute(prefs, flute);
 *   NVSStore::loadFlute(prefs, flute);
 *   NVSStore::savePressure(prefs, pressure);
 *   NVSStore::loadPressure(prefs, pressure);
 *
 * L'instance Preferences est passée en argument pour éviter le partage
 * d'état (chaque appel begin/end est encapsulé).
 */

#ifndef NVS_STORE_H
#define NVS_STORE_H

#include <Preferences.h>
#include "NVSKeys.h"
#include "Flute.h"
#include "PressureControl.h"
#include "settings.h"

namespace NVSStore {

  inline void saveFlute(Preferences& prefs, Flute* f) {
    if (!f) return;
    prefs.begin(NVSKeys::fluteNamespace(f->id()).c_str(), false);
    prefs.putUChar (NVSKeys::MIDI_CHANNEL, f->midiChannel());
    prefs.putUChar (NVSKeys::NOTE_MIN,     f->noteMin());
    prefs.putUChar (NVSKeys::NOTE_MAX,     f->noteMax());
    prefs.putBool  (NVSKeys::ENABLED,      f->isEnabled());
    prefs.putBool  (NVSKeys::MUTED,        f->isMuted());
    prefs.putFloat (NVSKeys::SPEED,        f->stepper().getSpeedMmS());
    prefs.putFloat (NVSKeys::ACCEL,        f->stepper().getAccelMmS2());
    prefs.putUChar (NVSKeys::PWM_FULL,     f->air().getPwmFull());
    prefs.putUChar (NVSKeys::PWM_HOLD,     f->air().getPwmHold());
    prefs.putUShort(NVSKeys::WAIT_DELAY,   f->air().getWaitDelay());
    prefs.putUShort(NVSKeys::LEGATO,       f->air().getLegatoMs());
    prefs.putUChar (NVSKeys::VEL_CURVE,    f->air().getVelocityCurve());
    prefs.putString(NVSKeys::CUSTOM_NAME,  f->customName());
    prefs.putUChar (NVSKeys::CC_BREATH,    f->getCcBreath());
    prefs.putUChar (NVSKeys::CC_EXPR,      f->getCcExpr());
    prefs.putUChar (NVSKeys::CC_VOLUME,    f->getCcVolume());
    prefs.putUChar (NVSKeys::CC_VIBRATO,   f->getCcVibrato());
    prefs.putUChar (NVSKeys::CC_SUSTAIN,   f->getCcSustain());
    prefs.putChar  (NVSKeys::TRANSPOSE,    f->getTranspose());
    prefs.putBool  (NVSKeys::USE_LUT,      f->stepper().getUseLUT());
    prefs.putBytes (NVSKeys::LUT_BLOB,     f->stepper().getLUT(),
                    sizeof(float) * MIDI_LUT_SIZE);
    prefs.end();
  }

  inline void loadFlute(Preferences& prefs, Flute* f) {
    if (!f) return;
    prefs.begin(NVSKeys::fluteNamespace(f->id()).c_str(), true);
    f->setMidiChannel(prefs.getUChar(NVSKeys::MIDI_CHANNEL, f->midiChannel()));
    uint8_t lo = prefs.getUChar(NVSKeys::NOTE_MIN, f->noteMin());
    uint8_t hi = prefs.getUChar(NVSKeys::NOTE_MAX, f->noteMax());
    f->setNoteRange(lo, hi);
    f->setEnabled(prefs.getBool(NVSKeys::ENABLED, true));
    f->setMuted  (prefs.getBool(NVSKeys::MUTED,   false));
    f->stepper().setSpeed       (prefs.getFloat (NVSKeys::SPEED,      DEFAULT_STEPPER_SPEED));
    f->stepper().setAcceleration(prefs.getFloat (NVSKeys::ACCEL,      DEFAULT_STEPPER_ACCEL));
    f->air().setPwmFull         (prefs.getUChar (NVSKeys::PWM_FULL,   DEFAULT_PWM_FULL));
    f->air().setPwmHold         (prefs.getUChar (NVSKeys::PWM_HOLD,   DEFAULT_PWM_HOLD));
    f->air().setWaitDelay       (prefs.getUShort(NVSKeys::WAIT_DELAY, DEFAULT_POSITION_WAIT));
    f->air().setLegatoMs        (prefs.getUShort(NVSKeys::LEGATO,     DEFAULT_LEGATO_THRESHOLD));
    f->air().setVelocityCurve   (prefs.getUChar (NVSKeys::VEL_CURVE,  f->air().getVelocityCurve()));
    String cn = prefs.getString(NVSKeys::CUSTOM_NAME, "");
    f->setCustomName(cn.c_str());
    f->setCcBreath (prefs.getUChar(NVSKeys::CC_BREATH,    2));
    f->setCcExpr   (prefs.getUChar(NVSKeys::CC_EXPR,     11));
    f->setCcVolume (prefs.getUChar(NVSKeys::CC_VOLUME,    7));
    f->setCcVibrato(prefs.getUChar(NVSKeys::CC_VIBRATO,   1));
    f->setCcSustain(prefs.getUChar(NVSKeys::CC_SUSTAIN,  64));
    f->setTranspose(prefs.getChar (NVSKeys::TRANSPOSE,    0));
    f->stepper().setUseLUT(prefs.getBool(NVSKeys::USE_LUT, false));
    if (prefs.getBytesLength(NVSKeys::LUT_BLOB) == sizeof(float) * MIDI_LUT_SIZE) {
      float buf[MIDI_LUT_SIZE];
      prefs.getBytes(NVSKeys::LUT_BLOB, buf, sizeof(buf));
      f->stepper().setLUT(buf);
    }
    prefs.end();
  }

  inline void savePressure(Preferences& prefs, PressureControl* p) {
    if (!p) return;
    prefs.begin("pressure", false);
    prefs.putFloat (NVSKeys::P_TARGET,  p->getTarget());
    prefs.putFloat (NVSKeys::P_MIN,     p->getMin());
    prefs.putFloat (NVSKeys::P_MAX,     p->getMax());
    prefs.putFloat (NVSKeys::P_SAFETY,  p->getSafety());
    prefs.putUShort(NVSKeys::P_FILL_MS, p->getMaxFillMs());
    prefs.end();
  }

  inline void loadPressure(Preferences& prefs, PressureControl* p) {
    if (!p) return;
    prefs.begin("pressure", true);
    p->setTarget   (prefs.getFloat (NVSKeys::P_TARGET,  p->getTarget()));
    p->setMin      (prefs.getFloat (NVSKeys::P_MIN,     p->getMin()));
    p->setMax      (prefs.getFloat (NVSKeys::P_MAX,     p->getMax()));
    p->setSafety   (prefs.getFloat (NVSKeys::P_SAFETY,  p->getSafety()));
    p->setMaxFillMs(prefs.getUShort(NVSKeys::P_FILL_MS, p->getMaxFillMs()));
    prefs.end();
  }
}

#endif // NVS_STORE_H
