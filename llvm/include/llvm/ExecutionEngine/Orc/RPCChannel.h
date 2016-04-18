// -*- c++ -*-

#ifndef LLVM_EXECUTIONENGINE_ORC_RPCCHANNEL_H
#define LLVM_EXECUTIONENGINE_ORC_RPCCHANNEL_H

#include "OrcError.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

#include <mutex>
#include <system_error>

namespace llvm {
namespace orc {
namespace remote {

/// Interface for byte-streams to be used with RPC.
class RPCChannel {
public:
  virtual ~RPCChannel() {}

  /// Read Size bytes from the stream into *Dst.
  virtual std::error_code readBytes(char *Dst, unsigned Size) = 0;

  /// Read size bytes from *Src and append them to the stream.
  virtual std::error_code appendBytes(const char *Src, unsigned Size) = 0;

  /// Flush the stream if possible.
  virtual std::error_code send() = 0;

  /// Get the lock for stream reading.
  std::mutex& getReadLock() { return readLock; }

  /// Get the lock for stream writing.
  std::mutex& getWriteLock() { return writeLock; }

private:
  std::mutex readLock, writeLock;
};

/// Notify the channel that we're starting a message send.
/// Locks the channel for writing.
inline std::error_code startSendMessage(RPCChannel &C) {
  C.getWriteLock().lock();
  return std::error_code();
}

/// Notify the channel that we're ending a message send.
/// Unlocks the channel for writing.
inline std::error_code endSendMessage(RPCChannel &C) {
  C.getWriteLock().unlock();
  return std::error_code();
}

/// Notify the channel that we're starting a message receive.
/// Locks the channel for reading.
inline std::error_code startReceiveMessage(RPCChannel &C) {
  C.getReadLock().lock();
  return std::error_code();
}

/// Notify the channel that we're ending a message receive.
/// Unlocks the channel for reading.
inline std::error_code endReceiveMessage(RPCChannel &C) {
  C.getReadLock().unlock();
  return std::error_code();
}

/// RPC channel serialization for a variadic list of arguments.
template <typename T, typename... Ts>
std::error_code serializeSeq(RPCChannel &C, const T &Arg, const Ts &... Args) {
  if (auto EC = serialize(C, Arg))
    return EC;
  return serializeSeq(C, Args...);
}

/// RPC channel serialization for an (empty) variadic list of arguments.
inline std::error_code serializeSeq(RPCChannel &C) {
  return std::error_code();
}

/// RPC channel deserialization for a variadic list of arguments.
template <typename T, typename... Ts>
std::error_code deserializeSeq(RPCChannel &C, T &Arg, Ts &... Args) {
  if (auto EC = deserialize(C, Arg))
    return EC;
  return deserializeSeq(C, Args...);
}

/// RPC channel serialization for an (empty) variadic list of arguments.
inline std::error_code deserializeSeq(RPCChannel &C) {
  return std::error_code();
}

/// RPC channel serialization for integer primitives.
template <typename T>
typename std::enable_if<
    std::is_same<T, uint64_t>::value || std::is_same<T, int64_t>::value ||
        std::is_same<T, uint32_t>::value || std::is_same<T, int32_t>::value ||
        std::is_same<T, uint16_t>::value || std::is_same<T, int16_t>::value ||
        std::is_same<T, uint8_t>::value || std::is_same<T, int8_t>::value,
    std::error_code>::type
serialize(RPCChannel &C, T V) {
  support::endian::byte_swap<T, support::big>(V);
  return C.appendBytes(reinterpret_cast<const char *>(&V), sizeof(T));
}

/// RPC channel deserialization for integer primitives.
template <typename T>
typename std::enable_if<
    std::is_same<T, uint64_t>::value || std::is_same<T, int64_t>::value ||
        std::is_same<T, uint32_t>::value || std::is_same<T, int32_t>::value ||
        std::is_same<T, uint16_t>::value || std::is_same<T, int16_t>::value ||
        std::is_same<T, uint8_t>::value || std::is_same<T, int8_t>::value,
    std::error_code>::type
deserialize(RPCChannel &C, T &V) {
  if (auto EC = C.readBytes(reinterpret_cast<char *>(&V), sizeof(T)))
    return EC;
  support::endian::byte_swap<T, support::big>(V);
  return std::error_code();
}

/// RPC channel serialization for enums.
template <typename T>
typename std::enable_if<std::is_enum<T>::value, std::error_code>::type
serialize(RPCChannel &C, T V) {
  return serialize(C, static_cast<typename std::underlying_type<T>::type>(V));
}

/// RPC channel deserialization for enums.
template <typename T>
typename std::enable_if<std::is_enum<T>::value, std::error_code>::type
deserialize(RPCChannel &C, T &V) {
  typename std::underlying_type<T>::type Tmp;
  std::error_code EC = deserialize(C, Tmp);
  V = static_cast<T>(Tmp);
  return EC;
}

/// RPC channel serialization for bools.
inline std::error_code serialize(RPCChannel &C, bool V) {
  uint8_t VN = V ? 1 : 0;
  return C.appendBytes(reinterpret_cast<const char *>(&VN), 1);
}

/// RPC channel deserialization for bools.
inline std::error_code deserialize(RPCChannel &C, bool &V) {
  uint8_t VN = 0;
  if (auto EC = C.readBytes(reinterpret_cast<char *>(&VN), 1))
    return EC;

  V = (VN != 0) ? true : false;
  return std::error_code();
}

/// RPC channel serialization for StringRefs.
/// Note: There is no corresponding deseralization for this, as StringRef
/// doesn't own its memory and so can't hold the deserialized data.
inline std::error_code serialize(RPCChannel &C, StringRef S) {
  if (auto EC = serialize(C, static_cast<uint64_t>(S.size())))
    return EC;
  return C.appendBytes((const char *)S.bytes_begin(), S.size());
}

/// RPC channel serialization for std::strings.
inline std::error_code serialize(RPCChannel &C, const std::string &S) {
  return serialize(C, StringRef(S));
}

/// RPC channel deserialization for std::strings.
inline std::error_code deserialize(RPCChannel &C, std::string &S) {
  uint64_t Count;
  if (auto EC = deserialize(C, Count))
    return EC;
  S.resize(Count);
  return C.readBytes(&S[0], Count);
}

// Serialization helper for std::tuple.
template <typename TupleT, size_t... Is>
inline std::error_code serializeTupleHelper(RPCChannel &C,
					    const TupleT &V,
					    llvm::index_sequence<Is...> _) {
  return serializeSeq(C, std::get<Is>(V)...);
}

/// RPC channel serialization for std::tuple.
template <typename... ArgTs>
inline std::error_code serialize(RPCChannel &C, const std::tuple<ArgTs...> &V) {
  return serializeTupleHelper(C, V, llvm::index_sequence_for<ArgTs...>());
}

// Serialization helper for std::tuple.
template <typename TupleT, size_t... Is>
inline std::error_code deserializeTupleHelper(RPCChannel &C,
					      TupleT &V,
					      llvm::index_sequence<Is...> _) {
  return deserializeSeq(C, std::get<Is>(V)...);
}

/// RPC channel deserialization for std::tuple.
template <typename... ArgTs>
inline std::error_code deserialize(RPCChannel &C, std::tuple<ArgTs...> &V) {
  return deserializeTupleHelper(C, V, llvm::index_sequence_for<ArgTs...>());
}

/// RPC channel serialization for ArrayRef<T>.
template <typename T>
std::error_code serialize(RPCChannel &C, const ArrayRef<T> &A) {
  if (auto EC = serialize(C, static_cast<uint64_t>(A.size())))
    return EC;

  for (const auto &E : A)
    if (auto EC = serialize(C, E))
      return EC;

  return std::error_code();
}

/// RPC channel serialization for std::array<T>.
template <typename T>
std::error_code serialize(RPCChannel &C, const std::vector<T> &V) {
  return serialize(C, ArrayRef<T>(V));
}

/// RPC channel deserialization for std::array<T>.
template <typename T>
std::error_code deserialize(RPCChannel &C, std::vector<T> &V) {
  uint64_t Count = 0;
  if (auto EC = deserialize(C, Count))
    return EC;

  V.resize(Count);
  for (auto &E : V)
    if (auto EC = deserialize(C, E))
      return EC;

  return std::error_code();
}

} // end namespace remote
} // end namespace orc
} // end namespace llvm

#endif
