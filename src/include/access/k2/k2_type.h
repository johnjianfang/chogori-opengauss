/*
MIT License

Copyright(c) 2022 Futurewei Cloud

    Permission is hereby granted,
    free of charge, to any person obtaining a copy of this software and associated documentation files(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions :

    The above copyright notice and this permission notice shall be included in all copies
    or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS",
    WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER
    LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#ifndef K2_TYPE_H
#define K2_TYPE_H

#include "access/htup.h"
#include "catalog/dependency.h"
#include "nodes/parsenodes.h"

#include "access/k2/pg_gate_typedefs.h"

/*
 * Constants for OIDs of supported Postgres native types (that do not have an
 * already declared constant in Postgres).
 */
#define K2PG_CHARARRAYOID 1002 /* char[] */
#define K2PG_TEXTARRAYOID 1009 /* text[] */
#define K2PG_ACLITEMARRAYOID 1034 /* aclitem[] */

extern const K2PgTypeEntity *K2PgDataTypeFromName(TypeName *typeName);
extern const K2PgTypeEntity *K2PgDataTypeFromOidMod(int attnum, Oid type_id);

/*
 * Returns true if we are allow the given type to be used for key columns such as primary key or
 * indexing key.
 */
bool K2PgDataTypeIsValidForKey(Oid type_id);

/*
 * Array of all type entities.
 */
void K2PgGetTypeTable(const K2PgTypeEntity **type_table, int *count);

#endif							/* K2_TYPE_H */