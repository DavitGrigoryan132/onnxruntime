// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/graph/op_identifier_utils.h"

#include "core/flatbuffers/flatbuffers_utils.h"
#include "core/flatbuffers/schema/ort.fbs.h"

namespace onnxruntime::fbs::utils {

#if !defined(ORT_MINIMAL_BUILD)

Status SaveOpIdentifierOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                                 const onnxruntime::OpIdentifier& op_id,
                                 flatbuffers::Offset<fbs::OpIdentifier>& fbs_op_id) {
  const auto fbs_domain = builder.CreateSharedString(op_id.domain);
  const auto fbs_op_type = builder.CreateSharedString(op_id.op_type);
  fbs_op_id = fbs::CreateOpIdentifier(builder, fbs_domain, fbs_op_type, op_id.since_version);
  return Status::OK();
}

#endif  // !defined(ORT_MINIMAL_BUILD)

Status LoadOpIdentifierOrtFormat(const fbs::OpIdentifier& fbs_op_id,
                                 onnxruntime::OpIdentifier& op_id) {
  const auto* fbs_domain = fbs_op_id.domain();
  ORT_FORMAT_RETURN_IF_NULL(fbs_domain, "domain");
  const auto* fbs_op_type = fbs_op_id.op_type();
  ORT_FORMAT_RETURN_IF_NULL(fbs_op_type, "op_type");
  op_id = onnxruntime::OpIdentifier{fbs_domain->str(), fbs_op_type->str(), fbs_op_id.since_version()};
  return Status::OK();
}

}  // namespace onnxruntime::fbs::utils
