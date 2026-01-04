#pragma once

namespace rediscoro::resp3 {

/// RESP3 protocol data types
enum class type {
  // Simple types
  simple_string,   // +
  simple_error,    // -
  integer,         // :
  double_type,     // ,
  boolean,         // #
  big_number,      // (
  null,            // _

  // Bulk types
  bulk_string,     // $
  bulk_error,      // !
  verbatim_string, // =

  // Aggregate types
  array,           // *
  map,             // %
  set,             // ~
  attribute,       // | (metadata for other types)
  push,            // >
};

}  // namespace rediscoro::resp3
