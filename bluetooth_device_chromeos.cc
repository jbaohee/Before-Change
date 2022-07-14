// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/bluetooth/bluetooth_device_chromeos.h"

#include "base/bind.h"
#include "base/metrics/histogram.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "chromeos/dbus/bluetooth_adapter_client.h"
#include "chromeos/dbus/bluetooth_device_client.h"
#include "chromeos/dbus/bluetooth_input_client.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "dbus/bus.h"
#include "device/bluetooth/bluetooth_adapter_chromeos.h"
#include "device/bluetooth/bluetooth_profile_chromeos.h"
#include "device/bluetooth/bluetooth_socket.h"
#include "third_party/cros_system_api/dbus/service_constants.h"

using device::BluetoothDevice;

namespace {

// Histogram enumerations for pairing results.
enum UMAPairingResult {
  UMA_PAIRING_RESULT_SUCCESS,
  UMA_PAIRING_RESULT_INPROGRESS,
  UMA_PAIRING_RESULT_FAILED,
  UMA_PAIRING_RESULT_AUTH_FAILED,
  UMA_PAIRING_RESULT_AUTH_CANCELED,
  UMA_PAIRING_RESULT_AUTH_REJECTED,
  UMA_PAIRING_RESULT_AUTH_TIMEOUT,
  UMA_PAIRING_RESULT_UNSUPPORTED_DEVICE,
  UMA_PAIRING_RESULT_UNKNOWN_ERROR,
  // NOTE: Add new pairing results immediately above this line. Make sure to
  // update the enum list in tools/histogram/histograms.xml accordinly.
  UMA_PAIRING_RESULT_COUNT
};

void ParseModalias(const dbus::ObjectPath& object_path,
                   uint16 *vendor_id,
                   uint16 *product_id,
                   uint16 *device_id) {
  chromeos::BluetoothDeviceClient::Properties* properties =
      chromeos::DBusThreadManager::Get()->GetBluetoothDeviceClient()->
          GetProperties(object_path);
  DCHECK(properties);

  std::string modalias = properties->modalias.value();
  if (StartsWithASCII(modalias, "usb:", false) && modalias.length() == 19) {
    // usb:vXXXXpXXXXdXXXX
    if (modalias[4] == 'v' && vendor_id != NULL) {
      uint64 component = 0;
      base::HexStringToUInt64(modalias.substr(5, 4), &component);
      *vendor_id = component;
    }

    if (modalias[9] == 'p' && product_id != NULL) {
      uint64 component = 0;
      base::HexStringToUInt64(modalias.substr(10, 4), &component);
      *product_id = component;
    }

    if (modalias[14] == 'd' && device_id != NULL) {
      uint64 component = 0;
      base::HexStringToUInt64(modalias.substr(15, 4), &component);
      *device_id = component;
    }
  }
}

void RecordPairingResult(BluetoothDevice::ConnectErrorCode error_code) {
  UMAPairingResult pairing_result;
  switch (error_code) {
    case BluetoothDevice::ERROR_INPROGRESS:
      pairing_result = UMA_PAIRING_RESULT_INPROGRESS;
      break;
    case BluetoothDevice::ERROR_FAILED:
      pairing_result = UMA_PAIRING_RESULT_FAILED;
      break;
    case BluetoothDevice::ERROR_AUTH_FAILED:
      pairing_result = UMA_PAIRING_RESULT_AUTH_FAILED;
      break;
    case BluetoothDevice::ERROR_AUTH_CANCELED:
      pairing_result = UMA_PAIRING_RESULT_AUTH_CANCELED;
      break;
    case BluetoothDevice::ERROR_AUTH_REJECTED:
      pairing_result = UMA_PAIRING_RESULT_AUTH_REJECTED;
      break;
    case BluetoothDevice::ERROR_AUTH_TIMEOUT:
      pairing_result = UMA_PAIRING_RESULT_AUTH_TIMEOUT;
      break;
    case BluetoothDevice::ERROR_UNSUPPORTED_DEVICE:
      pairing_result = UMA_PAIRING_RESULT_UNSUPPORTED_DEVICE;
      break;
    default:
      pairing_result = UMA_PAIRING_RESULT_UNKNOWN_ERROR;
  }

  UMA_HISTOGRAM_ENUMERATION("Bluetooth.PairingResult",
                            pairing_result,
                            UMA_PAIRING_RESULT_COUNT);
}

}  // namespace

namespace chromeos {

BluetoothDeviceChromeOS::BluetoothDeviceChromeOS(
    BluetoothAdapterChromeOS* adapter,
    const dbus::ObjectPath& object_path)
    : adapter_(adapter),
      object_path_(object_path),
      num_connecting_calls_(0),
      weak_ptr_factory_(this) {
}

BluetoothDeviceChromeOS::~BluetoothDeviceChromeOS() {
}

uint32 BluetoothDeviceChromeOS::GetBluetoothClass() const {
  BluetoothDeviceClient::Properties* properties =
      DBusThreadManager::Get()->GetBluetoothDeviceClient()->
          GetProperties(object_path_);
  DCHECK(properties);

  return properties->bluetooth_class.value();
}

std::string BluetoothDeviceChromeOS::GetDeviceName() const {
  BluetoothDeviceClient::Properties* properties =
      DBusThreadManager::Get()->GetBluetoothDeviceClient()->
          GetProperties(object_path_);
  DCHECK(properties);

  return properties->alias.value();
}

std::string BluetoothDeviceChromeOS::GetAddress() const {
  BluetoothDeviceClient::Properties* properties =
      DBusThreadManager::Get()->GetBluetoothDeviceClient()->
          GetProperties(object_path_);
  DCHECK(properties);

  return properties->address.value();
}

uint16 BluetoothDeviceChromeOS::GetVendorID() const {
  uint16 vendor_id  = 0;
  ParseModalias(object_path_, &vendor_id, NULL, NULL);
  return vendor_id;
}

uint16 BluetoothDeviceChromeOS::GetProductID() const {
  uint16 product_id  = 0;
  ParseModalias(object_path_, NULL, &product_id, NULL);
  return product_id;
}

uint16 BluetoothDeviceChromeOS::GetDeviceID() const {
  uint16 device_id  = 0;
  ParseModalias(object_path_, NULL, NULL, &device_id);
  return device_id;
}

bool BluetoothDeviceChromeOS::IsPaired() const {
  BluetoothDeviceClient::Properties* properties =
      DBusThreadManager::Get()->GetBluetoothDeviceClient()->
          GetProperties(object_path_);
  DCHECK(properties);

  // Trusted devices are devices that don't support pairing but that the
  // user has explicitly connected; it makes no sense for UI purposes to
  // treat them differently from each other.
  return properties->paired.value() || properties->trusted.value();
}

bool BluetoothDeviceChromeOS::IsConnected() const {
  BluetoothDeviceClient::Properties* properties =
      DBusThreadManager::Get()->GetBluetoothDeviceClient()->
          GetProperties(object_path_);
  DCHECK(properties);

  return properties->connected.value();
}

bool BluetoothDeviceChromeOS::IsConnectable() const {
  BluetoothInputClient::Properties* input_properties =
      DBusThreadManager::Get()->GetBluetoothInputClient()->
          GetProperties(object_path_);
  // GetProperties returns NULL when the device does not implement the given
  // interface. Non HID devices are normally connectable.
  if (!input_properties)
    return true;

  return input_properties->reconnect_mode.value() != "device";
}

bool BluetoothDeviceChromeOS::IsConnecting() const {
  return num_connecting_calls_ > 0;
}

BluetoothDeviceChromeOS::ServiceList BluetoothDeviceChromeOS::GetServices()
    const {
  BluetoothDeviceClient::Properties* properties =
      DBusThreadManager::Get()->GetBluetoothDeviceClient()->
          GetProperties(object_path_);
  DCHECK(properties);

  return properties->uuids.value();
}

void BluetoothDeviceChromeOS::GetServiceRecords(
    const ServiceRecordsCallback& callback,
    const ErrorCallback& error_callback) {
  // TODO(keybuk): not implemented; remove
  error_callback.Run();
}

void BluetoothDeviceChromeOS::ProvidesServiceWithName(
    const std::string& name,
    const ProvidesServiceCallback& callback) {
  // TODO(keybuk): not implemented; remove
  callback.Run(false);
}

bool BluetoothDeviceChromeOS::ExpectingPinCode() const {
  return pairing_context_.get() && pairing_context_->ExpectingPinCode();
}

bool BluetoothDeviceChromeOS::ExpectingPasskey() const {
  return pairing_context_.get() && pairing_context_->ExpectingPasskey();
}

bool BluetoothDeviceChromeOS::ExpectingConfirmation() const {
  return pairing_context_.get() && pairing_context_->ExpectingConfirmation();
}

void BluetoothDeviceChromeOS::Connect(
    BluetoothDevice::PairingDelegate* pairing_delegate,
    const base::Closure& callback,
    const ConnectErrorCallback& error_callback) {
  if (num_connecting_calls_++ == 0)
    adapter_->NotifyDeviceChanged(this);

  VLOG(1) << object_path_.value() << ": Connecting, " << num_connecting_calls_
          << " in progress";

  if (IsPaired() || !pairing_delegate || !IsPairable()) {
    // No need to pair, or unable to, skip straight to connection.
    ConnectInternal(false, callback, error_callback);
  } else {
    // Initiate high-security connection with pairing.
    DCHECK(!pairing_context_);
    pairing_context_.reset(
        new BluetoothAdapterChromeOS::PairingContext(pairing_delegate));

    DBusThreadManager::Get()->GetBluetoothDeviceClient()->
        Pair(object_path_,
             base::Bind(&BluetoothDeviceChromeOS::OnPair,
                        weak_ptr_factory_.GetWeakPtr(),
                        callback, error_callback),
             base::Bind(&BluetoothDeviceChromeOS::OnPairError,
                        weak_ptr_factory_.GetWeakPtr(),
                        error_callback));
  }
}

void BluetoothDeviceChromeOS::SetPinCode(const std::string& pincode) {
  if (!pairing_context_.get())
    return;

  pairing_context_->SetPinCode(pincode);
}

void BluetoothDeviceChromeOS::SetPasskey(uint32 passkey) {
  if (!pairing_context_.get())
    return;

  pairing_context_->SetPasskey(passkey);
}

void BluetoothDeviceChromeOS::ConfirmPairing() {
  if (!pairing_context_.get())
    return;

  pairing_context_->ConfirmPairing();
}

void BluetoothDeviceChromeOS::RejectPairing() {
  if (!pairing_context_.get())
    return;

  pairing_context_->RejectPairing();
}

void BluetoothDeviceChromeOS::CancelPairing() {
  // If there wasn't a callback in progress that we can reply to then we
  // have to send a CancelPairing() to the device instead.
  if (!pairing_context_.get() || !pairing_context_->CancelPairing()) {
    VLOG(1) << object_path_.value() << ": No pairing context or callback. "
            << "Sending explicit cancel";
    DBusThreadManager::Get()->GetBluetoothDeviceClient()->
        CancelPairing(
            object_path_,
            base::Bind(&base::DoNothing),
            base::Bind(&BluetoothDeviceChromeOS::OnCancelPairingError,
                       weak_ptr_factory_.GetWeakPtr()));

    // Since there's no calback to this method, it's possible that the pairing
    // delegate is going to be freed before things complete, so clear out the
    // context holding it.
    pairing_context_.reset();
  }
}

void BluetoothDeviceChromeOS::Disconnect(const base::Closure& callback,
                                         const ErrorCallback& error_callback) {
  VLOG(1) << object_path_.value() << ": Disconnecting";
  DBusThreadManager::Get()->GetBluetoothDeviceClient()->
      Disconnect(
          object_path_,
          base::Bind(&BluetoothDeviceChromeOS::OnDisconnect,
                     weak_ptr_factory_.GetWeakPtr(),
                     callback),
          base::Bind(&BluetoothDeviceChromeOS::OnDisconnectError,
                     weak_ptr_factory_.GetWeakPtr(),
                     error_callback));
}

void BluetoothDeviceChromeOS::Forget(const ErrorCallback& error_callback) {
  VLOG(1) << object_path_.value() << ": Removing device";
  DBusThreadManager::Get()->GetBluetoothAdapterClient()->
      RemoveDevice(
          adapter_->object_path_,
          object_path_,
          base::Bind(&base::DoNothing),
          base::Bind(&BluetoothDeviceChromeOS::OnForgetError,
                     weak_ptr_factory_.GetWeakPtr(),
                     error_callback));
}

void BluetoothDeviceChromeOS::ConnectToService(
    const std::string& service_uuid,
    const SocketCallback& callback) {
  // TODO(keybuk): implement
  callback.Run(scoped_refptr<device::BluetoothSocket>());
}

void BluetoothDeviceChromeOS::ConnectToProfile(
    device::BluetoothProfile* profile,
    const base::Closure& callback,
    const ErrorCallback& error_callback) {
  BluetoothProfileChromeOS* profile_chromeos =
      static_cast<BluetoothProfileChromeOS*>(profile);
  VLOG(1) << object_path_.value() << ": Connecting profile: "
          << profile_chromeos->uuid();
  DBusThreadManager::Get()->GetBluetoothDeviceClient()->
      ConnectProfile(
          object_path_,
          profile_chromeos->uuid(),
          base::Bind(
              &BluetoothDeviceChromeOS::OnConnectProfile,
              weak_ptr_factory_.GetWeakPtr(),
              profile,
              callback),
          base::Bind(
              &BluetoothDeviceChromeOS::OnConnectProfileError,
              weak_ptr_factory_.GetWeakPtr(),
              profile,
              error_callback));
}

void BluetoothDeviceChromeOS::SetOutOfBandPairingData(
    const device::BluetoothOutOfBandPairingData& data,
    const base::Closure& callback,
    const ErrorCallback& error_callback) {
  // TODO(keybuk): implement
  error_callback.Run();
}

void BluetoothDeviceChromeOS::ClearOutOfBandPairingData(
    const base::Closure& callback,
    const ErrorCallback& error_callback) {
  // TODO(keybuk): implement
  error_callback.Run();
}

void BluetoothDeviceChromeOS::ConnectInternal(
    bool after_pairing,
    const base::Closure& callback,
    const ConnectErrorCallback& error_callback) {
  VLOG(1) << object_path_.value() << ": Connecting";
  DBusThreadManager::Get()->GetBluetoothDeviceClient()->
      Connect(
          object_path_,
          base::Bind(&BluetoothDeviceChromeOS::OnConnect,
                     weak_ptr_factory_.GetWeakPtr(),
                     after_pairing,
                     callback),
          base::Bind(&BluetoothDeviceChromeOS::OnConnectError,
                     weak_ptr_factory_.GetWeakPtr(),
                     after_pairing,
                     error_callback));
}

void BluetoothDeviceChromeOS::OnConnect(bool after_pairing,
                                        const base::Closure& callback) {
  if (--num_connecting_calls_ == 0)
    adapter_->NotifyDeviceChanged(this);

  DCHECK(num_connecting_calls_ >= 0);
  VLOG(1) << object_path_.value() << ": Connected, " << num_connecting_calls_
        << " still in progress";

  SetTrusted();

  if (after_pairing)
    UMA_HISTOGRAM_ENUMERATION("Bluetooth.PairingResult",
                              UMA_PAIRING_RESULT_SUCCESS,
                              UMA_PAIRING_RESULT_COUNT);

  callback.Run();
}

void BluetoothDeviceChromeOS::OnConnectError(
    bool after_pairing,
    const ConnectErrorCallback& error_callback,
    const std::string& error_name,
    const std::string& error_message) {
  if (--num_connecting_calls_ == 0)
    adapter_->NotifyDeviceChanged(this);

  DCHECK(num_connecting_calls_ >= 0);
  LOG(WARNING) << object_path_.value() << ": Failed to connect device: "
               << error_name << ": " << error_message;
  VLOG(1) << object_path_.value() << ": " << num_connecting_calls_
          << " still in progress";

  // Determine the error code from error_name.
  ConnectErrorCode error_code = ERROR_UNKNOWN;
  if (error_name == bluetooth_device::kErrorFailed) {
    error_code = ERROR_FAILED;
  } else if (error_name == bluetooth_device::kErrorInProgress) {
    error_code = ERROR_INPROGRESS;
  } else if (error_name == bluetooth_device::kErrorNotSupported) {
    error_code = ERROR_UNSUPPORTED_DEVICE;
  }

  if (after_pairing)
    RecordPairingResult(error_code);
  error_callback.Run(error_code);
}

void BluetoothDeviceChromeOS::OnPair(
    const base::Closure& callback,
    const ConnectErrorCallback& error_callback) {
  VLOG(1) << object_path_.value() << ": Paired";

  pairing_context_.reset();

  SetTrusted();
  ConnectInternal(true, callback, error_callback);
}

void BluetoothDeviceChromeOS::OnPairError(
    const ConnectErrorCallback& error_callback,
    const std::string& error_name,
    const std::string& error_message) {
  if (--num_connecting_calls_ == 0)
    adapter_->NotifyDeviceChanged(this);

  DCHECK(num_connecting_calls_ >= 0);
  LOG(WARNING) << object_path_.value() << ": Failed to pair device: "
               << error_name << ": " << error_message;
  VLOG(1) << object_path_.value() << ": " << num_connecting_calls_
          << " still in progress";

  pairing_context_.reset();

  // Determine the error code from error_name.
  ConnectErrorCode error_code = ERROR_UNKNOWN;
  if (error_name == bluetooth_device::kErrorConnectionAttemptFailed) {
    error_code = ERROR_FAILED;
  } else if (error_name == bluetooth_device::kErrorFailed) {
    error_code = ERROR_FAILED;
  } else if (error_name == bluetooth_device::kErrorAuthenticationFailed) {
    error_code = ERROR_AUTH_FAILED;
  } else if (error_name == bluetooth_device::kErrorAuthenticationCanceled) {
    error_code = ERROR_AUTH_CANCELED;
  } else if (error_name == bluetooth_device::kErrorAuthenticationRejected) {
    error_code = ERROR_AUTH_REJECTED;
  } else if (error_name == bluetooth_device::kErrorAuthenticationTimeout) {
    error_code = ERROR_AUTH_TIMEOUT;
  }

  RecordPairingResult(error_code);
  error_callback.Run(error_code);
}

void BluetoothDeviceChromeOS::OnCancelPairingError(
    const std::string& error_name,
    const std::string& error_message) {
  LOG(WARNING) << object_path_.value() << ": Failed to cancel pairing: "
               << error_name << ": " << error_message;
}

void BluetoothDeviceChromeOS::SetTrusted() {
  // Unconditionally send the property change, rather than checking the value
  // first; there's no harm in doing this and it solves any race conditions
  // with the property becoming true or false and this call happening before
  // we get the D-Bus signal about the earlier change.
  DBusThreadManager::Get()->GetBluetoothDeviceClient()->
      GetProperties(object_path_)->trusted.Set(
          true,
          base::Bind(&BluetoothDeviceChromeOS::OnSetTrusted,
                     weak_ptr_factory_.GetWeakPtr()));
}

void BluetoothDeviceChromeOS::OnSetTrusted(bool success) {
  LOG_IF(WARNING, !success) << object_path_.value()
                            << ": Failed to set device as trusted";
}

void BluetoothDeviceChromeOS::OnDisconnect(const base::Closure& callback) {
  VLOG(1) << object_path_.value() << ": Disconnected";
  callback.Run();
}

void BluetoothDeviceChromeOS::OnDisconnectError(
    const ErrorCallback& error_callback,
    const std::string& error_name,
    const std::string& error_message) {
  LOG(WARNING) << object_path_.value() << ": Failed to disconnect device: "
               << error_name << ": " << error_message;
  error_callback.Run();
}

void BluetoothDeviceChromeOS::OnForgetError(
    const ErrorCallback& error_callback,
    const std::string& error_name,
    const std::string& error_message) {
  LOG(WARNING) << object_path_.value() << ": Failed to remove device: "
               << error_name << ": " << error_message;
  error_callback.Run();
}

void BluetoothDeviceChromeOS::OnConnectProfile(
    device::BluetoothProfile* profile,
    const base::Closure& callback) {
  BluetoothProfileChromeOS* profile_chromeos =
      static_cast<BluetoothProfileChromeOS*>(profile);
  VLOG(1) << object_path_.value() << ": Profile connected: "
          << profile_chromeos->uuid();
  callback.Run();
}

void BluetoothDeviceChromeOS::OnConnectProfileError(
    device::BluetoothProfile* profile,
    const ErrorCallback& error_callback,
    const std::string& error_name,
    const std::string& error_message) {
  BluetoothProfileChromeOS* profile_chromeos =
      static_cast<BluetoothProfileChromeOS*>(profile);
  VLOG(1) << object_path_.value() << ": Profile connection failed: "
          << profile_chromeos->uuid() << ": "
          << error_name << ": " << error_message;
  error_callback.Run();
}

}  // namespace chromeos
