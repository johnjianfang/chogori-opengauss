#
# Copyright (c) 2020 Huawei Technologies Co.,Ltd.
#
# openGauss is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#
#         http://license.coscl.org.cn/MulanPSL2
#
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
#
# -------------------------------------------------------------------------
#
# IDENTIFICATION
#    src/gausskernel/cbb/extension/connector/Makefile
#
# -------------------------------------------------------------------------

MODULE_big = connector

OBJS = connector.o odbc_connector.o odbc.o
SUBDIRS =

LDFLAGS_SL += $(filter -lm, $(LIBS))
LDFLAGS_SL += -ldl

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/gausskernel/cbb/extension/connector
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
#include $(top_srcdir)/src/backend/extension/extension-global.mk
endif

odbc.o:
	$(CC) $(CXXFLAGS) $(CPPFLAGS) -std=c++11 -c odbc.cpp

include $(top_srcdir)/src/gausskernel/common.mk
