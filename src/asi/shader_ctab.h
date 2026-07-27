#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace asi {

enum class ShaderCtabParseResult {
  NoCtab,
  Valid,
  Malformed,
};

struct ShaderMatrixContract {
  bool hasWorld = false;
  bool hasWorldViewProjection = false;
  uint16_t worldRegister = 0;
  uint16_t worldViewProjectionRegister = 0;
};

namespace shader_ctab_detail {

inline bool ReadU16(const uint8_t* bytes,
                    size_t byteCount,
                    size_t offset,
                    uint16_t* value) {
  if (!bytes || !value || offset > byteCount ||
      byteCount - offset < sizeof(uint16_t))
    return false;
  std::memcpy(value, bytes + offset, sizeof(uint16_t));
  return true;
}

inline bool ReadU32(const uint8_t* bytes,
                    size_t byteCount,
                    size_t offset,
                    uint32_t* value) {
  if (!bytes || !value || offset > byteCount ||
      byteCount - offset < sizeof(uint32_t))
    return false;
  std::memcpy(value, bytes + offset, sizeof(uint32_t));
  return true;
}

inline bool NameEquals(const uint8_t* table,
                       size_t tableBytes,
                       uint32_t offset,
                       const char* expected) {
  if (!table || !expected || offset >= tableBytes)
    return false;
  size_t index = offset;
  size_t expectedIndex = 0;
  while (index < tableBytes) {
    const char actual = static_cast<char>(table[index++]);
    if (actual != expected[expectedIndex])
      return false;
    if (actual == '\0')
      return true;
    ++expectedIndex;
  }
  return false;
}

inline bool ParseMatrixConstant(const uint8_t* table,
                                size_t tableBytes,
                                size_t infoOffset,
                                uint16_t* registerIndex,
                                uint32_t* nameOffset) {
  uint16_t registerSet = 0;
  uint16_t parsedRegisterIndex = 0;
  uint16_t registerCount = 0;
  uint32_t parsedNameOffset = 0;
  uint32_t typeOffset = 0;
  if (!ReadU32(table, tableBytes, infoOffset, &parsedNameOffset) ||
      !ReadU16(table, tableBytes, infoOffset + 4u, &registerSet) ||
      !ReadU16(table, tableBytes, infoOffset + 6u,
               &parsedRegisterIndex) ||
      !ReadU16(table, tableBytes, infoOffset + 8u, &registerCount) ||
      !ReadU32(table, tableBytes, infoOffset + 12u, &typeOffset))
    return false;

  uint16_t parameterClass = 0;
  uint16_t parameterType = 0;
  uint16_t rows = 0;
  uint16_t columns = 0;
  uint16_t elements = 0;
  uint16_t structMembers = 0;
  if (!ReadU16(table, tableBytes, typeOffset, &parameterClass) ||
      !ReadU16(table, tableBytes, typeOffset + 2u, &parameterType) ||
      !ReadU16(table, tableBytes, typeOffset + 4u, &rows) ||
      !ReadU16(table, tableBytes, typeOffset + 6u, &columns) ||
      !ReadU16(table, tableBytes, typeOffset + 8u, &elements) ||
      !ReadU16(table, tableBytes, typeOffset + 10u, &structMembers))
    return false;

  // D3DXRS_FLOAT4=2, D3DXPC_MATRIX_ROWS=2, D3DXPT_FLOAT=3.
  if (registerSet != 2u || registerCount != 4u ||
      parameterClass != 2u || parameterType != 3u ||
      rows != 4u || columns != 4u || elements != 1u ||
      structMembers != 0u)
    return false;
  if (registerIndex)
    *registerIndex = parsedRegisterIndex;
  if (nameOffset)
    *nameOffset = parsedNameOffset;
  return true;
}

}  // namespace shader_ctab_detail

// Parses only Microsoft's embedded CTAB declaration. It does not infer matrix
// registers by scanning float constants, and therefore cannot silently patch an
// unknown shader layout.
inline ShaderCtabParseResult ParseShaderMatrixContract(
    const void* shaderBytecode,
    size_t byteCount,
    ShaderMatrixContract* output) {
  if (!output)
    return ShaderCtabParseResult::Malformed;
  *output = {};
  if (!shaderBytecode || byteCount < 3u * sizeof(uint32_t))
    return ShaderCtabParseResult::NoCtab;

  const uint8_t* bytes = static_cast<const uint8_t*>(shaderBytecode);
  const size_t wordCount = byteCount / sizeof(uint32_t);
  constexpr uint32_t CommentOpcode = 0x0000fffeu;
  constexpr uint32_t CtabFourCc = 0x42415443u;
  bool foundCtab = false;

  for (size_t word = 0; word + 2u < wordCount; ++word) {
    uint32_t token = 0;
    uint32_t fourCc = 0;
    std::memcpy(&token, bytes + word * sizeof(uint32_t), sizeof(token));
    if ((token & 0xffffu) != CommentOpcode)
      continue;
    const uint32_t commentWords = (token >> 16u) & 0x7fffu;
    if (commentWords < 2u ||
        commentWords > wordCount - word - 1u)
      continue;
    std::memcpy(
        &fourCc,
        bytes + (word + 1u) * sizeof(uint32_t),
        sizeof(fourCc));
    if (fourCc != CtabFourCc)
      continue;
    foundCtab = true;

    // CTAB offsets are byte offsets from the table header immediately after
    // the CTAB fourcc, not from the shader or comment token.
    const uint8_t* table =
        bytes + (word + 2u) * sizeof(uint32_t);
    const size_t tableBytes =
        static_cast<size_t>(commentWords - 1u) * sizeof(uint32_t);
    uint32_t headerBytes = 0;
    uint32_t constantCount = 0;
    uint32_t constantInfoOffset = 0;
    if (!shader_ctab_detail::ReadU32(
            table, tableBytes, 0u, &headerBytes) ||
        !shader_ctab_detail::ReadU32(
            table, tableBytes, 12u, &constantCount) ||
        !shader_ctab_detail::ReadU32(
            table, tableBytes, 16u, &constantInfoOffset) ||
        headerBytes != 28u || constantCount > 512u ||
        constantInfoOffset > tableBytes ||
        static_cast<size_t>(constantCount) >
            (tableBytes - constantInfoOffset) / 20u)
      return ShaderCtabParseResult::Malformed;

    for (uint32_t constant = 0; constant < constantCount; ++constant) {
      const size_t infoOffset =
          static_cast<size_t>(constantInfoOffset) +
          static_cast<size_t>(constant) * 20u;
      uint16_t registerIndex = 0;
      uint32_t nameOffset = 0;
      uint32_t rawNameOffset = 0;
      if (!shader_ctab_detail::ReadU32(
              table, tableBytes, infoOffset, &rawNameOffset))
        return ShaderCtabParseResult::Malformed;
      const bool isWorld =
          shader_ctab_detail::NameEquals(
              table, tableBytes, rawNameOffset, "gWorld");
      const bool isWorldViewProjection =
          shader_ctab_detail::NameEquals(
              table, tableBytes, rawNameOffset, "gWorldViewProj");
      if (!isWorld && !isWorldViewProjection)
        continue;
      if (!shader_ctab_detail::ParseMatrixConstant(
              table, tableBytes, infoOffset, &registerIndex, &nameOffset) ||
          nameOffset != rawNameOffset)
        return ShaderCtabParseResult::Malformed;
      if (isWorld) {
        if (output->hasWorld)
          return ShaderCtabParseResult::Malformed;
        output->hasWorld = true;
        output->worldRegister = registerIndex;
      } else {
        if (output->hasWorldViewProjection)
          return ShaderCtabParseResult::Malformed;
        output->hasWorldViewProjection = true;
        output->worldViewProjectionRegister = registerIndex;
      }
    }
    return ShaderCtabParseResult::Valid;
  }
  return foundCtab
      ? ShaderCtabParseResult::Malformed
      : ShaderCtabParseResult::NoCtab;
}

}  // namespace asi
