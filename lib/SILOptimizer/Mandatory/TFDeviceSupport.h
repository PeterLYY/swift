//===--- TFDeviceSupport.h - TensorFlow device management -------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This defines the abstractions for representing TF device types, device
// placement for graph_op insts, and device partitioning API.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_TFDEVICESUPPORT_H
#define SWIFT_SILOPTIMIZER_TFDEVICESUPPORT_H

#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILConstants.h"
#include "swift/SIL/SILLocation.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"

namespace swift {
namespace tf {

/// The device of a tfop instruction (and its output tensors, if any).
enum class DeviceType {
  INVALID,
  CPU,
  GPU,
  TPU,
  /// Indicates this instruction should run on all devices in
  /// `GraphFunctionDeviceInfo::usedDeviceTypes`. For example, a promoted
  /// scalar will run on all such devices, in case it is a loop iteration count
  /// and the loop runs on all devices.
  ALL,
};

/// Must be kepted in sync with the enum class above.
static const int NUM_DEVICE_TYPES = 5;

class DevicePartitionerImpl;
struct GraphOperationInfo;

static const char DEFAULT_CPU_DEVICE[] = "/device:CPU:0";
static const char DEFAULT_GPU_DEVICE[] = "/device:GPU:0";
static const char DEFAULT_TPU_DEVICE[] = "TPU_SYSTEM";
// This is a pseudo-device that only exist in the SIL code generated by
// TFPartition and GraphPartitioner, and will be replaced with real devices in
// TFGraphLowering.
static const char ALL_DEVICES[] = "ALL_DEVICES";

// We assume the following special attr names do not occur in the regular
// attributes of any TF ops.
static const char DEVICE_ATTR[] = "__device";
// This pseudo-attribute is propagated from a tfop inst to TensorTransfer, and
// then to D2D send/recv insts. When lowering to TF graph, the pseudo-attribute
// is used when creating TPU infeed/outfeed ops, and is dropped when creating
// other TF ops (e.g. a "Const" op).
static const char SHAPE_ARRAY_ATTR[] = "__shapes";

static inline DeviceType getOpDeviceType(llvm::StringRef device) {
  if (device.str() == DEFAULT_CPU_DEVICE)
    return DeviceType::CPU;
  if (device.str() == DEFAULT_GPU_DEVICE)
    return DeviceType::GPU;
  if (device.str() == DEFAULT_TPU_DEVICE)
    return DeviceType::TPU;
  if (device.str() == ALL_DEVICES)
    return DeviceType::ALL;

  // FIXME: Consider also supporting variants of the device string, such as
  // "CPU:0".
  llvm_unreachable("Unknown device type");
}

/// The returned string is compatible with TF device name used in TF graphs.
static inline std::string getDeviceString(DeviceType deviceType) {
  switch (deviceType) {
  case DeviceType::CPU:
    return DEFAULT_CPU_DEVICE;
  case DeviceType::GPU:
    return DEFAULT_GPU_DEVICE;
  case DeviceType::TPU:
    return DEFAULT_TPU_DEVICE;
  case DeviceType::ALL:
    return ALL_DEVICES;
  case DeviceType::INVALID:
    llvm_unreachable("Unsupported device type");
  }
}

StringRef getDeviceString(const GraphOperationInfo &graphOpInfo);

DeviceType getDeviceType(const GraphOperationInfo &graphOpInfo);

/// The returned string can be used to construct SIL function names.
static inline std::string getDeviceShortName(DeviceType deviceType) {
  switch (deviceType) {
  case DeviceType::CPU:
    return "CPU";
  case DeviceType::GPU:
    return "GPU";
  case DeviceType::TPU:
    return "TPU";
  case DeviceType::ALL:
    return "ALL";
  case DeviceType::INVALID:
    llvm_unreachable("Unsupported device type");
  }
}

/// Returns true if `attrName` is SHAPE_ARRAY_ATTR, `attrValue` is an array of
/// TensorShape-typed elements.
bool isShapeArrayPseudoAttr(llvm::StringRef name, SymbolicValue attrValue);

/// This struct holds information about the deviceInfo of the graph we are
/// generating.
struct GraphFunctionDeviceInfo {
  const DeviceType primaryDeviceType;
  const bool isTPUInfeedEnabled;

  unsigned numUsedDeviceTypes;

  /// This class provides iterator support for a set of device types represented
  /// in a boolean array.
  class DeviceTypeMgr {
    const bool *usedDeviceTypes;

  public:
    /// `usedDeviceTypes` must have exactly NUM_DEVICE_TYPES elements, and the
    /// elements corresponding to DeviceType::INVALID and DeviceType::ALL must
    /// not be set.
    DeviceTypeMgr(const bool *usedDeviceTypes)
        : usedDeviceTypes(usedDeviceTypes) {}

    class iterator
        : public std::iterator<std::input_iterator_tag, // iterator_category
                               DeviceType,              // value_type
                               long,                    // difference_type
                               const DeviceType *,      // pointer
                               DeviceType               // reference
                               > {
      const bool *usedDeviceTypes;
      unsigned deviceIdx;

    public:
      explicit iterator(const bool *usedDeviceTypes, unsigned deviceIdx)
          : usedDeviceTypes(usedDeviceTypes), deviceIdx(deviceIdx) {
        assert(deviceIdx >= 0);
        assert(deviceIdx <= NUM_DEVICE_TYPES);
      }
      iterator &operator++() {
        while (++deviceIdx < NUM_DEVICE_TYPES) {
          if (!usedDeviceTypes[deviceIdx])
            continue;
          auto ret = (DeviceType)deviceIdx;
          assert(ret != DeviceType::INVALID);
          assert(ret != DeviceType::ALL);
          return *this;
        }
        return *this;
      }
      iterator operator++(int) {
        iterator retval = *this;
        ++(*this);
        return retval;
      }
      bool operator==(iterator other) const {
        return usedDeviceTypes == other.usedDeviceTypes &&
               deviceIdx == other.deviceIdx;
      }
      bool operator!=(iterator other) const { return !(*this == other); }
      reference operator*() const { return (DeviceType)deviceIdx; }
    };
    iterator begin() {
      auto ret = iterator(usedDeviceTypes, 0);
      // We know the first entry in `usedDeviceTypes` is not valid, so we use ++
      // to return the first valid entry.
      return ++ret;
    }
    iterator end() { return iterator(usedDeviceTypes, NUM_DEVICE_TYPES); }
  };

  DeviceTypeMgr getUsedDeviceTypes() const {
    return DeviceTypeMgr(usedDeviceTypes);
  }

  /// Return the deviceInfo for the specified function.
  static GraphFunctionDeviceInfo getForFunction(SILFunction &fn,
                                                bool removeConfigInst);

  void markDeviceUsed(DeviceType device) {
    assert(device != DeviceType::INVALID);
    if (device == DeviceType::ALL || usedDeviceTypes[(unsigned)device])
      return;
    usedDeviceTypes[(unsigned)device] = true;
    ++numUsedDeviceTypes;
  }

  // Choose a device for the graphOpInst under construction, extend `attributes`
  // accordingly with the device attribute, and track the chosen device in
  // `usedDeviceTypes`.
  //
  // If `opDevice` is already set, respects that device choice. Otherwise,
  // chooses a device based on this deviceInfo and op kernel device
  // availability.
  //
  // Caller should avoid adding duplicate device attributes (e.g. calling
  // handleDevicePlacement() multiple times when creating the same graph_op
  // inst). Otherwise SILVerifier will fail on that graph_op inst.
  void
  handleDevicePlacement(llvm::StringRef opType, llvm::StringRef opDevice,
                        ASTContext &ctx,
                        SmallVectorImpl<GraphOperationAttribute> &attributes);

private:
  GraphFunctionDeviceInfo(DeviceType primaryDeviceType, bool isTPUInfeedEnabled)
      : primaryDeviceType(primaryDeviceType),
        isTPUInfeedEnabled(isTPUInfeedEnabled) {
    assert(primaryDeviceType != DeviceType::ALL);
    memset(usedDeviceTypes, 0, sizeof(usedDeviceTypes));
    usedDeviceTypes[(unsigned)primaryDeviceType] = true;
    numUsedDeviceTypes = 1;
  }

  DeviceType chooseDevice(llvm::StringRef opType) const;

  // Actual TF devices involved in the tensor computation.
  // It cannot contain DeviceType::ALL.
  bool usedDeviceTypes[NUM_DEVICE_TYPES];
};

/// Partitions an accelerator SIL function into a set of per-device SIL
/// functions.
class DevicePartitioner {
  DevicePartitionerImpl *impl;

public:
  DevicePartitioner(SILFunction &srcFn,
                    const GraphFunctionDeviceInfo &deviceInfo,
                    int &nextTensorTransferId);

  ~DevicePartitioner();

  /// Returns a function extracted from `srcFn`, specialized on `deviceType`.
  ///
  /// For example, say `fn` returns a+b, where a and b and constant tensors,
  /// and a is placed on GPU.
  /// - The extracted function for GPU device has the constant node a, fed
  /// into
  ///   a _Send() node to CPU.
  /// - The extracted function for CPU device has _Recv node from GPU to read
  ///   a, and adds its output with const tensor b to produce the sum result.
  SILFunction *extractFunctionForDevice(DeviceType deviceType);
};

} // end namespace tf
} // end namespace swift

#endif