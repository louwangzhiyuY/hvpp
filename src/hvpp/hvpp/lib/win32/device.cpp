#include "../device.h"

#include <ntddk.h>

//
// Definition is located in win32/driver.cpp.
//
extern PDRIVER_OBJECT GlobalDriverObject;

#define HVPP_DEVICE_TAG     'vdvh'
#define MAX_BUFFER_SIZE     64

//
// Private implementation.
//
typedef struct _DEVICE_IMPL
{
  PDEVICE_OBJECT DeviceObject;

  UNICODE_STRING DeviceLink;
  WCHAR DeviceLinkBuffer[MAX_BUFFER_SIZE + sizeof(L"\\DosDevices\\") - 1];
} DEVICE_IMPL, *PDEVICE_IMPL;

auto device::create() noexcept -> error_code_t
{
  error_code_t err;

  size_t name_length = strlen(name());

  if (name_length >= MAX_BUFFER_SIZE)
  {
    return make_error_code_t(std::errc::invalid_argument);
  }

  PDEVICE_IMPL DeviceImpl = (PDEVICE_IMPL)
                            ExAllocatePoolWithTag(NonPagedPool,
                                                  sizeof(DEVICE_IMPL),
                                                  HVPP_DEVICE_TAG);

  if (!DeviceImpl)
  {
    err = make_error_code_t(std::errc::not_enough_memory);
    goto Error;
  }

  //
  // Create ANSI_STRING for name.
  //
  ANSI_STRING AnsiName;
  AnsiName.Length           = (USHORT)name_length;
  AnsiName.MaximumLength    = (USHORT)name_length;
  AnsiName.Buffer           = (PCHAR)name();

  //
  // Convert ANSI_STRING to UNICODE_STRING.
  //
  WCHAR UnicodeNameBuffer[MAX_BUFFER_SIZE];
  UNICODE_STRING UnicodeName;
  UnicodeName.Length = 0;
  UnicodeName.MaximumLength = sizeof(UnicodeNameBuffer);
  UnicodeName.Buffer = UnicodeNameBuffer;
  RtlAnsiStringToUnicodeString(&UnicodeName, &AnsiName, FALSE);

  //
  // Create DeviceName.
  //
  WCHAR DeviceNameBuffer[MAX_BUFFER_SIZE + sizeof(L"\\Device\\")];
  UNICODE_STRING DeviceName;
  DeviceName.Length = 0;
  DeviceName.MaximumLength = sizeof(DeviceNameBuffer);
  DeviceName.Buffer = DeviceNameBuffer;
  RtlAppendUnicodeToString(&DeviceName, L"\\Device\\");
  RtlAppendUnicodeStringToString(&DeviceName, &UnicodeName);

  //
  // Create DeviceLink.
  //
  DeviceImpl->DeviceLink.Length = 0;
  DeviceImpl->DeviceLink.MaximumLength = sizeof(DeviceImpl->DeviceLinkBuffer);
  DeviceImpl->DeviceLink.Buffer = DeviceImpl->DeviceLinkBuffer;
  RtlAppendUnicodeToString(&DeviceImpl->DeviceLink, L"\\DosDevices\\");
  RtlAppendUnicodeStringToString(&DeviceImpl->DeviceLink, &UnicodeName);

  //
  // Create the device.
  //
  NTSTATUS Status;
  Status = IoCreateDevice(GlobalDriverObject,
                          sizeof(device*),
                          &DeviceName,
                          FILE_DEVICE_UNKNOWN,
                          0,
                          FALSE,
                          &DeviceImpl->DeviceObject);

  if (!NT_SUCCESS(Status))
  {
    err = make_error_code_t(std::errc::not_enough_memory);
    goto Error;
  }

  //
  // Assign pointer to this instance to the
  // DeviceExtension field.
  //
  *(device**)(DeviceImpl->DeviceObject->DeviceExtension) = this;

  //
  // Tell the kernel we want to use buffered I/O.
  //
  DeviceImpl->DeviceObject->Flags |= DO_BUFFERED_IO;

  //
  // Tell I/O manager the device is initialized.
  //
  DeviceImpl->DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

  //
  // Create symbolic link.
  //
  Status = IoCreateSymbolicLink(&DeviceImpl->DeviceLink, &DeviceName);

  if (!NT_SUCCESS(Status))
  {
    err = make_error_code_t(std::errc::not_enough_memory);
    goto Error;
  }

  //
  // Success path.
  //
  impl_ = DeviceImpl;
  return err;

  //
  // Error path.
  //
Error:
  if (DeviceImpl->DeviceObject)
  {
    IoDeleteDevice(DeviceImpl->DeviceObject);
  }

  ExFreePoolWithTag(DeviceImpl, HVPP_DEVICE_TAG);
  return err;
}

void device::destroy() noexcept
{
  PDEVICE_IMPL DeviceImpl = (PDEVICE_IMPL)impl_;

  if (DeviceImpl)
  {
    IoDeleteSymbolicLink(&DeviceImpl->DeviceLink);
    IoDeleteDevice(DeviceImpl->DeviceObject);

    ExFreePoolWithTag(DeviceImpl, HVPP_DEVICE_TAG);

    impl_ = nullptr;
  }
}

error_code_t device::copy_from_user(void* buffer_to, const void* buffer_from, size_t length) noexcept
{
  __try
  {
    ProbeForRead((volatile void*)buffer_from, length, sizeof(UCHAR));
    RtlCopyMemory(buffer_to, buffer_from, length);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return make_error_code_t(std::errc::bad_address);
  }

  return error_code_t{};
}

error_code_t device::copy_to_user(void* buffer_to, const void* buffer_from, size_t length) noexcept
{
  __try
  {
    ProbeForWrite(buffer_to, length, sizeof(UCHAR));
    RtlCopyMemory(buffer_to, buffer_from, length);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return make_error_code_t(std::errc::bad_address);
  }

  return error_code_t{};
}
