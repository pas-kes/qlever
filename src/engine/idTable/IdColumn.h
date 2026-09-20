//
// Created by Pascal Keßler on 20.09.26.
//

#ifndef QLEVER_SRC_ENGINE_IDTABLE_IDCOLUMN_H
#define QLEVER_SRC_ENGINE_IDTABLE_IDCOLUMN_H

#include "../../util/Enums.h"
#include "../../util/SourceLocation.h"
#include "backports/span.h"
#include "global/Id.h"

/**
 * This type aliases should replace the old span<Id> and span<const Id> types so it could be replaced easier by the new 64+8 Bit Id
 * without changing half of sourcecode.
 */

using IdColumn = ql::span<Id>;
using ConstIdColumn = ql::span<const Id>;

#endif  // QLEVER_SRC_ENGINE_IDTABLE_IDCOLUMN_H
