// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/audio_mixer_alsa.h"

#include <alsa/asoundlib.h>

#include "base/logging.h"
#include "base/task.h"

namespace chromeos {

// Connect to the ALSA mixer using their simple element API.  Init is performed
// asynchronously on the worker thread.
//
// To get a wider range and finer control over volume levels, first the Master
// level is set, then if the PCM element exists, the total level is refined by
// adjusting that as well.  If the PCM element has more volume steps, it allows
// for finer granularity in the total volume.

// TODO(davej): Serialize volume/mute to preserve settings when restarting.

typedef long alsa_long_t;  // 'long' is required for ALSA API calls.

namespace {

const char* kMasterVolume = "Master";
const char* kPCMVolume = "PCM";
const double kDefaultMinVolume = -90.0;
const double kDefaultMaxVolume = 0.0;

}  // namespace

AudioMixerAlsa::AudioMixerAlsa()
    : min_volume_(kDefaultMinVolume),
      max_volume_(kDefaultMaxVolume),
      save_volume_(0),
      mixer_state_(UNINITIALIZED),
      alsa_mixer_(NULL),
      elem_master_(NULL),
      elem_pcm_(NULL) {
}

AudioMixerAlsa::~AudioMixerAlsa() {
  FreeAlsaMixer();
  if (thread_ != NULL) {
    thread_->Stop();
    thread_.reset();
  }
}

void AudioMixerAlsa::Init(InitDoneCallback* callback) {
  DCHECK(callback);
  if (!InitThread()) {
    callback->Run(false);
    delete callback;
    return;
  }

  // Post the task of starting up, which can block for 200-500ms,
  // so best not to do it on the caller's thread.
  thread_->message_loop()->PostTask(FROM_HERE,
      NewRunnableMethod(this, &AudioMixerAlsa::DoInit, callback));
}

bool AudioMixerAlsa::InitSync() {
  if (!InitThread())
    return false;
  return InitializeAlsaMixer();
}

double AudioMixerAlsa::GetVolumeDb() const {
  AutoLock lock(mixer_state_lock_);
  if (mixer_state_ != READY)
    return kSilenceDb;

  return DoGetVolumeDb_Locked();
}

bool AudioMixerAlsa::GetVolumeLimits(double* vol_min, double* vol_max) {
  AutoLock lock(mixer_state_lock_);
  if (mixer_state_ != READY)
    return false;
  if (vol_min)
    *vol_min = min_volume_;
  if (vol_max)
    *vol_max = max_volume_;
  return true;
}

void AudioMixerAlsa::SetVolumeDb(double vol_db) {
  AutoLock lock(mixer_state_lock_);
  if (mixer_state_ != READY)
    return;
  DoSetVolumeDb_Locked(vol_db);
}

bool AudioMixerAlsa::IsMute() const {
  AutoLock lock(mixer_state_lock_);
  if (mixer_state_ != READY)
    return false;
  return GetElementMuted_Locked(elem_master_);
}

void AudioMixerAlsa::SetMute(bool mute) {
  AutoLock lock(mixer_state_lock_);
  if (mixer_state_ != READY)
    return;

  // Set volume to minimum on mute, since switching the element off does not
  // always mute as it should.

  // TODO(davej): Setting volume to minimum can be removed once switching the
  //              element off can be guaranteed to work.

  bool old_value = GetElementMuted_Locked(elem_master_);

  if (old_value != mute) {
    if (mute) {
      save_volume_ = DoGetVolumeDb_Locked();
      DoSetVolumeDb_Locked(min_volume_);
    } else {
      DoSetVolumeDb_Locked(save_volume_);
    }
  }

  SetElementMuted_Locked(elem_master_, mute);
  if (elem_pcm_)
    SetElementMuted_Locked(elem_pcm_, mute);
}

AudioMixer::State AudioMixerAlsa::GetState() const {
  AutoLock lock(mixer_state_lock_);
  // If we think it's ready, verify it is actually so.
  if ((mixer_state_ == READY) && (alsa_mixer_ == NULL))
    mixer_state_ = IN_ERROR;
  return mixer_state_;
}

////////////////////////////////////////////////////////////////////////////////
// Private functions follow

void AudioMixerAlsa::DoInit(InitDoneCallback* callback) {
  bool success = InitializeAlsaMixer();

  if (callback) {
    callback->Run(success);
    delete callback;
  }
}

bool AudioMixerAlsa::InitThread() {
  AutoLock lock(mixer_state_lock_);

  if (mixer_state_ != UNINITIALIZED)
    return false;

  if (thread_ == NULL) {
    thread_.reset(new base::Thread("AudioMixerAlsa"));
    if (!thread_->Start()) {
      thread_.reset();
      return false;
    }
  }

  mixer_state_ = INITIALIZING;
  return true;
}

bool AudioMixerAlsa::InitializeAlsaMixer() {
  AutoLock lock(mixer_state_lock_);
  if (mixer_state_ != INITIALIZING)
    return false;

  int err;
  snd_mixer_t* handle = NULL;
  const char* card = "default";

  if ((err = snd_mixer_open(&handle, 0)) < 0) {
    LOG(ERROR) << "ALSA mixer " << card << " open error: " << snd_strerror(err);
    return false;
  }

  if ((err = snd_mixer_attach(handle, card)) < 0) {
    LOG(ERROR) << "ALSA Attach to card " << card << " failed: "
               << snd_strerror(err);
    snd_mixer_close(handle);
    return false;
  }

  if ((err = snd_mixer_selem_register(handle, NULL, NULL)) < 0) {
    LOG(ERROR) << "ALSA mixer register error: " << snd_strerror(err);
    snd_mixer_close(handle);
    return false;
  }

  if ((err = snd_mixer_load(handle)) < 0) {
    LOG(ERROR) << "ALSA mixer " << card << " load error: %s"
               << snd_strerror(err);
    snd_mixer_close(handle);
    return false;
  }

  VLOG(1) << "Opened ALSA mixer " << card << " OK";

  elem_master_ = FindElementWithName_Locked(handle, kMasterVolume);
  if (elem_master_) {
    alsa_long_t long_lo, long_hi;
    snd_mixer_selem_get_playback_dB_range(elem_master_, &long_lo, &long_hi);
    min_volume_ = static_cast<double>(long_lo) / 100.0;
    max_volume_ = static_cast<double>(long_hi) / 100.0;
  } else {
    LOG(ERROR) << "Cannot find 'Master' ALSA mixer element on " << card;
    snd_mixer_close(handle);
    return false;
  }

  elem_pcm_ = FindElementWithName_Locked(handle, kPCMVolume);
  if (elem_pcm_) {
    alsa_long_t long_lo, long_hi;
    snd_mixer_selem_get_playback_dB_range(elem_pcm_, &long_lo, &long_hi);
    min_volume_ += static_cast<double>(long_lo) / 100.0;
    max_volume_ += static_cast<double>(long_hi) / 100.0;
  }

  VLOG(1) << "ALSA volume range is " << min_volume_ << " dB to "
          << max_volume_ << " dB";

  alsa_mixer_ = handle;
  mixer_state_ = READY;
  return true;
}

void AudioMixerAlsa::FreeAlsaMixer() {
  AutoLock lock(mixer_state_lock_);
  mixer_state_ = SHUTTING_DOWN;
  if (alsa_mixer_) {
    snd_mixer_close(alsa_mixer_);
    alsa_mixer_ = NULL;
  }
}

double AudioMixerAlsa::DoGetVolumeDb_Locked() const {
  double vol_total = 0.0;
  GetElementVolume_Locked(elem_master_, &vol_total);

  double vol_pcm = 0.0;
  if (elem_pcm_ && (GetElementVolume_Locked(elem_pcm_, &vol_pcm)))
    vol_total += vol_pcm;

  return vol_total;
}

void AudioMixerAlsa::DoSetVolumeDb_Locked(double vol_db) {
  double actual_vol = 0.0;

  // If a PCM volume slider exists, then first set the Master volume to the
  // nearest volume >= requested volume, then adjust PCM volume down to get
  // closer to the requested volume.

  if (elem_pcm_) {
    SetElementVolume_Locked(elem_master_, vol_db, &actual_vol, 0.9999f);
    SetElementVolume_Locked(elem_pcm_, vol_db - actual_vol, NULL, 0.5f);
  } else {
    SetElementVolume_Locked(elem_master_, vol_db, NULL, 0.5f);
  }
}

snd_mixer_elem_t* AudioMixerAlsa::FindElementWithName_Locked(
    snd_mixer_t* handle,
    const char* element_name) const {
  snd_mixer_selem_id_t* sid;

  // Using id_malloc/id_free API instead of id_alloca since the latter gives the
  // warning: the address of 'sid' will always evaluate as 'true'
  if (snd_mixer_selem_id_malloc(&sid))
    return NULL;

  snd_mixer_selem_id_set_index(sid, 0);
  snd_mixer_selem_id_set_name(sid, element_name);
  snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);
  if (!elem) {
    LOG(ERROR) << "ALSA unable to find simple control "
               << snd_mixer_selem_id_get_name(sid);
  }

  snd_mixer_selem_id_free(sid);
  return elem;
}

bool AudioMixerAlsa::GetElementVolume_Locked(snd_mixer_elem_t* elem,
                                             double* current_vol) const {
  alsa_long_t long_vol = 0;
  snd_mixer_selem_get_playback_dB(elem,
                                  static_cast<snd_mixer_selem_channel_id_t>(0),
                                  &long_vol);
  *current_vol = static_cast<double>(long_vol) / 100.0;

  return true;
}

bool AudioMixerAlsa::SetElementVolume_Locked(snd_mixer_elem_t* elem,
                                             double new_vol,
                                             double* actual_vol,
                                             double rounding_bias) {
  alsa_long_t vol_lo = 0;
  alsa_long_t vol_hi = 0;
  snd_mixer_selem_get_playback_volume_range(elem, &vol_lo, &vol_hi);
  alsa_long_t vol_range = vol_hi - vol_lo;
  if (vol_range <= 0)
    return false;

  alsa_long_t db_lo_int = 0;
  alsa_long_t db_hi_int = 0;
  snd_mixer_selem_get_playback_dB_range(elem, &db_lo_int, &db_hi_int);
  double db_lo = static_cast<double>(db_lo_int) / 100.0;
  double db_hi = static_cast<double>(db_hi_int) / 100.0;
  double db_step = static_cast<double>(db_hi - db_lo) / vol_range;
  if (db_step <= 0.0)
    return false;

  if (new_vol < db_lo)
    new_vol = db_lo;

  alsa_long_t value = static_cast<alsa_long_t>(rounding_bias +
      (new_vol - db_lo) / db_step) + vol_lo;
  snd_mixer_selem_set_playback_volume_all(elem, value);

  VLOG(1) << "Set volume " << snd_mixer_selem_get_name(elem)
          << " to " << new_vol << " ==> " << (value - vol_lo) * db_step + db_lo
          << " dB";

  if (actual_vol) {
    alsa_long_t volume;
    snd_mixer_selem_get_playback_volume(
        elem,
        static_cast<snd_mixer_selem_channel_id_t>(0),
        &volume);
    *actual_vol = db_lo + (volume - vol_lo) * db_step;

    VLOG(1) << "Actual volume " << snd_mixer_selem_get_name(elem)
            << " now " << *actual_vol << " dB";
  }
  return true;
}

bool AudioMixerAlsa::GetElementMuted_Locked(snd_mixer_elem_t* elem) const {
  int enabled;
  snd_mixer_selem_get_playback_switch(
      elem,
      static_cast<snd_mixer_selem_channel_id_t>(0),
      &enabled);
  return (enabled) ? false : true;
}

void AudioMixerAlsa::SetElementMuted_Locked(snd_mixer_elem_t* elem, bool mute) {
  int enabled = mute ? 0 : 1;
  snd_mixer_selem_set_playback_switch_all(elem, enabled);

  VLOG(1) << "Set playback switch " << snd_mixer_selem_get_name(elem)
          << " to " << enabled;
}

}  // namespace chromeos

