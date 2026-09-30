// SPDX-License-Identifier: GPL-2.0-or-later
//
// DolphinSwitch::Storage for the SD card only. NaGa's launcher also mounts USB drives
// (libusbhsfs) and SMB shares (libsmb2); until those libraries are part of this build, no USB
// locations are reported and SMB shares can't be mounted.

#include "DolphinSwitch/Storage.h"

namespace DolphinSwitch::Storage
{
bool InitializeUsb(std::string* error)
{
  if (error)
    *error = "USB storage isn't supported yet";
  return false;
}

std::uint64_t UsbStatusGeneration()
{
  return 0;
}

void SetUsbStatusCallback(UsbStatusCallback, void*)
{
}

UsbSnapshot GetUsbSnapshot()
{
  return {};
}

std::vector<Location> ListUsbLocations()
{
  return {};
}

std::string ResolveUsbPath(const std::string&)
{
  return {};
}

bool SafelyEjectUsb(const std::string&, std::string* error)
{
  if (error)
    *error = "USB storage isn't supported yet";
  return false;
}

bool MountSmb(const SmbShare&, std::string* error, const std::atomic_bool*)
{
  if (error)
    *error = "SMB shares aren't supported yet";
  return false;
}

bool UnmountSmb(const std::string&)
{
  return false;
}

bool IsSmbMounted(const std::string&)
{
  return false;
}

SmbConnectionState GetSmbConnectionState(const std::string&)
{
  return SmbConnectionState::Disconnected;
}

bool ReconnectSmb(const std::string&, std::string* error, const std::atomic_bool*)
{
  if (error)
    *error = "SMB shares aren't supported yet";
  return false;
}

std::string SmbRootPath(const std::string&)
{
  return {};
}

std::string SmbBrowsePath(const SmbShare&)
{
  return {};
}

void Shutdown()
{
}
}  // namespace DolphinSwitch::Storage
