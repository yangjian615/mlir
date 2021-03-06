//===- Attributes.cpp - MLIR Affine Expr Classes --------------------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "mlir/IR/Attributes.h"
#include "AttributeDetail.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Types.h"

using namespace mlir;
using namespace mlir::detail;

Attribute::Kind Attribute::getKind() const { return attr->kind; }

bool Attribute::isOrContainsFunction() const {
  return attr->isOrContainsFunctionCache;
}

// Given an attribute that could refer to a function attribute in the remapping
// table, walk it and rewrite it to use the mapped function.  If it doesn't
// refer to anything in the table, then it is returned unmodified.
Attribute Attribute::remapFunctionAttrs(
    const llvm::DenseMap<Attribute, FunctionAttr> &remappingTable,
    MLIRContext *context) const {
  // Most attributes are trivially unrelated to function attributes, skip them
  // rapidly.
  if (!isOrContainsFunction())
    return *this;

  // If we have a function attribute, remap it.
  if (auto fnAttr = this->dyn_cast<FunctionAttr>()) {
    auto it = remappingTable.find(fnAttr);
    return it != remappingTable.end() ? it->second : *this;
  }

  // Otherwise, we must have an array attribute, remap the elements.
  auto arrayAttr = this->cast<ArrayAttr>();
  SmallVector<Attribute, 8> remappedElts;
  bool anyChange = false;
  for (auto elt : arrayAttr.getValue()) {
    auto newElt = elt.remapFunctionAttrs(remappingTable, context);
    remappedElts.push_back(newElt);
    anyChange |= (elt != newElt);
  }

  if (!anyChange)
    return *this;

  return ArrayAttr::get(remappedElts, context);
}

//===----------------------------------------------------------------------===//
// NumericAttr
//===----------------------------------------------------------------------===//

Type NumericAttr::getType() const {
  if (auto boolAttr = dyn_cast<BoolAttr>())
    return boolAttr.getType();
  if (auto intAttr = dyn_cast<IntegerAttr>())
    return intAttr.getType();
  if (auto floatAttr = dyn_cast<FloatAttr>())
    return floatAttr.getType();
  if (auto elemAttr = dyn_cast<ElementsAttr>())
    return elemAttr.getType();

  llvm_unreachable("unhandled NumericAttr subclass");
}

bool NumericAttr::kindof(Kind kind) {
  return BoolAttr::kindof(kind) || IntegerAttr::kindof(kind) ||
         FloatAttr::kindof(kind) || ElementsAttr::kindof(kind);
}

//===----------------------------------------------------------------------===//
// BoolAttr
//===----------------------------------------------------------------------===//

bool BoolAttr::getValue() const { return static_cast<ImplType *>(attr)->value; }

Type BoolAttr::getType() const { return static_cast<ImplType *>(attr)->type; }

//===----------------------------------------------------------------------===//
// IntegerAttr
//===----------------------------------------------------------------------===//

APInt IntegerAttr::getValue() const {
  return static_cast<ImplType *>(attr)->getValue();
}

int64_t IntegerAttr::getInt() const { return getValue().getSExtValue(); }

Type IntegerAttr::getType() const {
  return static_cast<ImplType *>(attr)->type;
}

//===----------------------------------------------------------------------===//
// FloatAttr
//===----------------------------------------------------------------------===//

APFloat FloatAttr::getValue() const {
  return static_cast<ImplType *>(attr)->getValue();
}

Type FloatAttr::getType() const { return static_cast<ImplType *>(attr)->type; }

double FloatAttr::getValueAsDouble() const {
  const auto &semantics = getType().cast<FloatType>().getFloatSemantics();
  auto value = getValue();
  bool losesInfo = false; // ignored
  if (&semantics != &APFloat::IEEEdouble()) {
    value.convert(APFloat::IEEEdouble(), APFloat::rmNearestTiesToEven,
                  &losesInfo);
  }
  return value.convertToDouble();
}

//===----------------------------------------------------------------------===//
// StringAttr
//===----------------------------------------------------------------------===//

StringRef StringAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

//===----------------------------------------------------------------------===//
// ArrayAttr
//===----------------------------------------------------------------------===//

ArrayRef<Attribute> ArrayAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

//===----------------------------------------------------------------------===//
// AffineMapAttr
//===----------------------------------------------------------------------===//

AffineMap AffineMapAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

//===----------------------------------------------------------------------===//
// IntegerSetAttr
//===----------------------------------------------------------------------===//

IntegerSet IntegerSetAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

//===----------------------------------------------------------------------===//
// TypeAttr
//===----------------------------------------------------------------------===//

Type TypeAttr::getValue() const { return static_cast<ImplType *>(attr)->value; }

//===----------------------------------------------------------------------===//
// FunctionAttr
//===----------------------------------------------------------------------===//

Function *FunctionAttr::getValue() const {
  return static_cast<ImplType *>(attr)->value;
}

FunctionType FunctionAttr::getType() const { return getValue()->getType(); }

//===----------------------------------------------------------------------===//
// ElementsAttr
//===----------------------------------------------------------------------===//

VectorOrTensorType ElementsAttr::getType() const {
  return static_cast<ImplType *>(attr)->type;
}

/// Return the value at the given index. If index does not refer to a valid
/// element, then a null attribute is returned.
Attribute ElementsAttr::getValue(ArrayRef<uint64_t> index) const {
  switch (getKind()) {
  case Attribute::Kind::SplatElements:
    return cast<SplatElementsAttr>().getValue();
  case Attribute::Kind::DenseFPElements:
  case Attribute::Kind::DenseIntElements:
    return cast<DenseElementsAttr>().getValue(index);
  case Attribute::Kind::OpaqueElements:
    return cast<OpaqueElementsAttr>().getValue(index);
  case Attribute::Kind::SparseElements:
    return cast<SparseElementsAttr>().getValue(index);
  default:
    llvm_unreachable("unknown ElementsAttr kind");
  }
}

//===----------------------------------------------------------------------===//
// SplatElementsAttr
//===----------------------------------------------------------------------===//

Attribute SplatElementsAttr::getValue() const {
  return static_cast<ImplType *>(attr)->elt;
}

//===----------------------------------------------------------------------===//
// RawElementIterator
//===----------------------------------------------------------------------===//

static size_t getDenseElementBitwidth(Type eltType) {
  // FIXME(b/121118307): using 64 bits for BF16 because it is currently stored
  // with double semantics.
  return eltType.isBF16() ? 64 : eltType.getIntOrFloatBitWidth();
}

/// Constructs a new iterator.
DenseElementsAttr::RawElementIterator::RawElementIterator(
    DenseElementsAttr attr, size_t index)
    : rawData(attr.getRawData().data()), index(index),
      bitWidth(getDenseElementBitwidth(attr.getType().getElementType())) {}

/// Accesses the raw APInt value at this iterator position.
APInt DenseElementsAttr::RawElementIterator::operator*() const {
  return readBits(rawData, index * bitWidth, bitWidth);
}

//===----------------------------------------------------------------------===//
// DenseElementsAttr
//===----------------------------------------------------------------------===//

/// Returns the number of elements held by this attribute.
size_t DenseElementsAttr::size() const { return getType().getNumElements(); }

/// Return the value at the given index. If index does not refer to a valid
/// element, then a null attribute is returned.
Attribute DenseElementsAttr::getValue(ArrayRef<uint64_t> index) const {
  auto type = getType();

  // Verify that the rank of the indices matches the held type.
  auto rank = type.getRank();
  if (rank != index.size())
    return Attribute();

  // Verify that all of the indices are within the shape dimensions.
  auto shape = type.getShape();
  for (unsigned i = 0; i != rank; ++i)
    if (shape[i] <= index[i])
      return Attribute();

  // Reduce the provided multidimensional index into a 1D index.
  uint64_t valueIndex = 0;
  uint64_t dimMultiplier = 1;
  for (auto i = rank - 1; i >= 0; --i) {
    valueIndex += index[i] * dimMultiplier;
    dimMultiplier *= shape[i];
  }

  // Return the element stored at the 1D index.
  auto elementType = getType().getElementType();
  size_t bitWidth = getDenseElementBitwidth(elementType);
  APInt rawValueData =
      readBits(getRawData().data(), valueIndex * bitWidth, bitWidth);

  // Convert the raw value data to an attribute value.
  switch (getKind()) {
  case Attribute::Kind::DenseIntElements:
    return IntegerAttr::get(elementType, rawValueData);
  case Attribute::Kind::DenseFPElements:
    return FloatAttr::get(
        elementType, APFloat(elementType.cast<FloatType>().getFloatSemantics(),
                             rawValueData));
  default:
    llvm_unreachable("unexpected element type");
  }
}

void DenseElementsAttr::getValues(SmallVectorImpl<Attribute> &values) const {
  auto elementType = getType().getElementType();
  switch (getKind()) {
  case Attribute::Kind::DenseIntElements: {
    // Get the raw APInt values.
    SmallVector<APInt, 8> intValues;
    cast<DenseIntElementsAttr>().getValues(intValues);

    // Convert each to an IntegerAttr.
    for (auto &intVal : intValues)
      values.push_back(IntegerAttr::get(elementType, intVal));
    return;
  }
  case Attribute::Kind::DenseFPElements: {
    // Get the raw APFloat values.
    SmallVector<APFloat, 8> floatValues;
    cast<DenseFPElementsAttr>().getValues(floatValues);

    // Convert each to an FloatAttr.
    for (auto &floatVal : floatValues)
      values.push_back(FloatAttr::get(elementType, floatVal));
    return;
  }
  default:
    llvm_unreachable("unexpected element type");
  }
}

ArrayRef<char> DenseElementsAttr::getRawData() const {
  return static_cast<ImplType *>(attr)->data;
}

// Constructs a dense elements attribute from an array of raw APInt values.
// Each APInt value is expected to have the same bitwidth as the element type
// of 'type'.
DenseElementsAttr DenseElementsAttr::get(VectorOrTensorType type,
                                         ArrayRef<APInt> values) {
  assert(values.size() == type.getNumElements() &&
         "expected 'values' to contain the same number of elements as 'type'");

  size_t bitWidth = getDenseElementBitwidth(type.getElementType());
  std::vector<char> elementData(APInt::getNumWords(bitWidth * values.size()) *
                                APInt::APINT_WORD_SIZE);
  for (unsigned i = 0, e = values.size(); i != e; ++i) {
    assert(values[i].getBitWidth() == bitWidth);
    writeBits(elementData.data(), i * bitWidth, values[i]);
  }
  return get(type, elementData);
}

/// Writes value to the bit position `bitPos` in array `rawData`. 'rawData' is
/// expected to be a 64-bit aligned storage address.
void DenseElementsAttr::writeBits(char *rawData, size_t bitPos, APInt value) {
  size_t bitWidth = value.getBitWidth();

  // If the bitwidth is 1 we just toggle the specific bit.
  if (bitWidth == 1) {
    auto *rawIntData = reinterpret_cast<uint64_t *>(rawData);
    if (value.isOneValue())
      APInt::tcSetBit(rawIntData, bitPos);
    else
      APInt::tcClearBit(rawIntData, bitPos);
    return;
  }

  // If the bit position and width are byte aligned, write the storage directly
  // to the data.
  if ((bitWidth % 8) == 0 && (bitPos % 8) == 0) {
    std::copy_n(reinterpret_cast<const char *>(value.getRawData()),
                bitWidth / 8, rawData + (bitPos / 8));
    return;
  }

  // Otherwise, convert the raw data into an APInt and insert the value at the
  // specified bit position.
  size_t totalWords = APInt::getNumWords((bitPos % 64) + bitWidth);
  llvm::MutableArrayRef<uint64_t> rawIntData(
      reinterpret_cast<uint64_t *>(rawData) + (bitPos / 64), totalWords);
  APInt tempStorage(totalWords * 64, rawIntData);
  tempStorage.insertBits(value, bitPos % 64);

  // Copy the value back to the raw data.
  std::copy_n(tempStorage.getRawData(), rawIntData.size(), rawIntData.data());
}

/// Reads the next `bitWidth` bits from the bit position `bitPos` in array
/// `rawData`. 'rawData' is expected to be a 64-bit aligned storage address.
APInt DenseElementsAttr::readBits(const char *rawData, size_t bitPos,
                                  size_t bitWidth) {
  // Reinterpret the raw data as a uint64_t word array and extract the value
  // starting at 'bitPos'.
  APInt result(bitWidth, 0);
  const uint64_t *intData = reinterpret_cast<const uint64_t *>(rawData);
  APInt::tcExtract(const_cast<uint64_t *>(result.getRawData()),
                   result.getNumWords(), intData, bitWidth, bitPos);
  return result;
}

//===----------------------------------------------------------------------===//
// DenseIntElementsAttr
//===----------------------------------------------------------------------===//

/// Constructs a dense integer elements attribute from an array of APInt
/// values. Each APInt value is expected to have the same bitwidth as the
/// element type of 'type'.
DenseIntElementsAttr DenseIntElementsAttr::get(VectorOrTensorType type,
                                               ArrayRef<APInt> values) {
  return DenseElementsAttr::get(type, values).cast<DenseIntElementsAttr>();
}

/// Constructs a dense integer elements attribute from an array of integer
/// values. Each value is expected to be within the bitwidth of the element
/// type of 'type'.
DenseIntElementsAttr DenseIntElementsAttr::get(VectorOrTensorType type,
                                               ArrayRef<int64_t> values) {
  auto eltType = type.getElementType();
  size_t bitWidth = eltType.isBF16() ? 64 : eltType.getIntOrFloatBitWidth();

  // Convert the raw integer values to APInt.
  SmallVector<APInt, 8> apIntValues;
  apIntValues.reserve(values.size());
  for (auto value : values)
    apIntValues.emplace_back(APInt(bitWidth, value));
  return get(type, apIntValues);
}

void DenseIntElementsAttr::getValues(SmallVectorImpl<APInt> &values) const {
  values.reserve(size());
  values.assign(raw_begin(), raw_end());
}

//===----------------------------------------------------------------------===//
// DenseFPElementsAttr
//===----------------------------------------------------------------------===//

DenseFPElementsAttr::ElementIterator::ElementIterator(
    const llvm::fltSemantics &smt, RawElementIterator it)
    : llvm::mapped_iterator<RawElementIterator,
                            std::function<APFloat(const APInt &)>>(
          it, [&](const APInt &val) { return APFloat(smt, val); }) {}

// Constructs a dense float elements attribute from an array of APFloat
// values. Each APFloat value is expected to have the same bitwidth as the
// element type of 'type'.
DenseFPElementsAttr DenseFPElementsAttr::get(VectorOrTensorType type,
                                             ArrayRef<APFloat> values) {
  // Convert the APFloat values to APInt and create a dense elements attribute.
  std::vector<APInt> intValues(values.size());
  for (unsigned i = 0, e = values.size(); i != e; ++i)
    intValues[i] = values[i].bitcastToAPInt();
  return DenseElementsAttr::get(type, intValues).cast<DenseFPElementsAttr>();
}

void DenseFPElementsAttr::getValues(SmallVectorImpl<APFloat> &values) const {
  values.reserve(size());
  values.assign(begin(), end());
}

/// Iterator access to the float element values.
DenseFPElementsAttr::iterator DenseFPElementsAttr::begin() const {
  auto elementType = getType().getElementType().cast<FloatType>();
  const auto &elementSemantics = elementType.getFloatSemantics();
  return {elementSemantics, raw_begin()};
}
DenseFPElementsAttr::iterator DenseFPElementsAttr::end() const {
  auto elementType = getType().getElementType().cast<FloatType>();
  const auto &elementSemantics = elementType.getFloatSemantics();
  return {elementSemantics, raw_end()};
}

//===----------------------------------------------------------------------===//
// OpaqueElementsAttr
//===----------------------------------------------------------------------===//

StringRef OpaqueElementsAttr::getValue() const {
  return static_cast<ImplType *>(attr)->bytes;
}

/// Return the value at the given index. If index does not refer to a valid
/// element, then a null attribute is returned.
Attribute OpaqueElementsAttr::getValue(ArrayRef<uint64_t> index) const {
  if (Dialect *dialect = getDialect())
    return dialect->extractElementHook(*this, index);
  return Attribute();
}

Dialect *OpaqueElementsAttr::getDialect() const {
  return static_cast<ImplType *>(attr)->dialect;
}

bool OpaqueElementsAttr::decode(ElementsAttr &result) {
  if (auto *d = getDialect())
    return d->decodeHook(*this, result);
  return true;
}

//===----------------------------------------------------------------------===//
// SparseElementsAttr
//===----------------------------------------------------------------------===//

DenseIntElementsAttr SparseElementsAttr::getIndices() const {
  return static_cast<ImplType *>(attr)->indices;
}

DenseElementsAttr SparseElementsAttr::getValues() const {
  return static_cast<ImplType *>(attr)->values;
}

/// Return the value of the element at the given index.
Attribute SparseElementsAttr::getValue(ArrayRef<uint64_t> index) const {
  auto type = getType();

  // Verify that the rank of the indices matches the held type.
  auto rank = type.getRank();
  if (rank != index.size())
    return Attribute();

  // The sparse indices are 64-bit integers, so we can reinterpret the raw data
  // as a 1-D index array.
  auto sparseIndices = getIndices();
  const uint64_t *sparseIndexValues =
      reinterpret_cast<const uint64_t *>(sparseIndices.getRawData().data());

  // Build a mapping between known indices and the offset of the stored element.
  llvm::SmallDenseMap<llvm::ArrayRef<uint64_t>, size_t> mappedIndices;
  auto numSparseIndices = sparseIndices.getType().getDimSize(0);
  for (size_t i = 0, e = numSparseIndices; i != e; ++i)
    mappedIndices.try_emplace(
        {sparseIndexValues + (i * rank), static_cast<size_t>(rank)}, i);

  // Look for the provided index key within the mapped indices. If the provided
  // index is not found, then return a zero attribute.
  auto it = mappedIndices.find(index);
  if (it == mappedIndices.end()) {
    auto eltType = type.getElementType();
    if (eltType.isa<FloatType>())
      return FloatAttr::get(eltType, 0);
    assert(eltType.isa<IntegerType>() && "unexpected element type");
    return IntegerAttr::get(eltType, 0);
  }

  // Otherwise, return the held sparse value element.
  return getValues().getValue(it->second);
}

//===----------------------------------------------------------------------===//
// NamedAttributeList
//===----------------------------------------------------------------------===//

NamedAttributeList::NamedAttributeList(MLIRContext *context,
                                       ArrayRef<NamedAttribute> attributes) {
  setAttrs(context, attributes);
}

/// Return all of the attributes on this operation.
ArrayRef<NamedAttribute> NamedAttributeList::getAttrs() const {
  return attrs ? attrs->getElements() : llvm::None;
}

/// Replace the held attributes with ones provided in 'newAttrs'.
void NamedAttributeList::setAttrs(MLIRContext *context,
                                  ArrayRef<NamedAttribute> attributes) {
  // Don't create an attribute list if there are no attributes.
  if (attributes.empty()) {
    attrs = nullptr;
    return;
  }

  assert(llvm::all_of(attributes,
                      [](const NamedAttribute &attr) { return attr.second; }) &&
         "attributes cannot have null entries");
  attrs = AttributeListStorage::get(attributes, context);
}

/// Return the specified attribute if present, null otherwise.
Attribute NamedAttributeList::get(StringRef name) const {
  for (auto elt : getAttrs())
    if (elt.first.is(name))
      return elt.second;
  return nullptr;
}
Attribute NamedAttributeList::get(Identifier name) const {
  for (auto elt : getAttrs())
    if (elt.first == name)
      return elt.second;
  return nullptr;
}

/// If the an attribute exists with the specified name, change it to the new
/// value.  Otherwise, add a new attribute with the specified name/value.
void NamedAttributeList::set(MLIRContext *context, Identifier name,
                             Attribute value) {
  assert(value && "attributes may never be null");

  // If we already have this attribute, replace it.
  auto origAttrs = getAttrs();
  SmallVector<NamedAttribute, 8> newAttrs(origAttrs.begin(), origAttrs.end());
  for (auto &elt : newAttrs)
    if (elt.first == name) {
      elt.second = value;
      attrs = AttributeListStorage::get(newAttrs, context);
      return;
    }

  // Otherwise, add it.
  newAttrs.push_back({name, value});
  attrs = AttributeListStorage::get(newAttrs, context);
}

/// Remove the attribute with the specified name if it exists.  The return
/// value indicates whether the attribute was present or not.
auto NamedAttributeList::remove(MLIRContext *context, Identifier name)
    -> RemoveResult {
  auto origAttrs = getAttrs();
  for (unsigned i = 0, e = origAttrs.size(); i != e; ++i) {
    if (origAttrs[i].first == name) {
      SmallVector<NamedAttribute, 8> newAttrs;
      newAttrs.reserve(origAttrs.size() - 1);
      newAttrs.append(origAttrs.begin(), origAttrs.begin() + i);
      newAttrs.append(origAttrs.begin() + i + 1, origAttrs.end());
      attrs = AttributeListStorage::get(newAttrs, context);
      return RemoveResult::Removed;
    }
  }
  return RemoveResult::NotFound;
}
