#----------------------------------------------------------------------------
#
# openGauss documentation makefile
#
# doc/src/sgml/Makefile
#
#----------------------------------------------------------------------------

# This makefile is for building and installing the documentation.
# When a release tarball is created, the documentation files are
# prepared using the distprep target.  In Git-based trees these files
# don't exist, unless explicitly built, so we skip the installation in
# that case.


# Make "html" the default target, since that is what most people tend
# to want to use.
html:

subdir = doc/src/sgml
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

MAKESGMLDIR = $(top_builddir)/src/common/pgxc/tools/makesgml
MAKESGML = $(MAKESGMLDIR)/makesgml


all: html man

distprep: html distprep-man


ifndef COLLATEINDEX
COLLATEINDEX = $(DOCBOOKSTYLE)/bin/collateindex.pl
endif

ifndef JADE
JADE = jade
endif
SGMLINCLUDE = -D . -D $(srcdir)

ifndef NSGMLS
NSGMLS = nsgmls
endif

ifndef OSX
OSX = osx
endif

ifndef XSLTPROC
XSLTPROC = xsltproc
endif

VERSION = 1.1

override XSLTPROCFLAGS += --stringparam pg.version '$(VERSION)'


GENERATED_SGML = bookindex.sgml version.sgml \
	features-supported.sgml features-unsupported.sgml errcodes-table.sgml

ALLSGMLIN := $(wildcard $(srcdir)/*.sgmlin $(srcdir)/ref/*.sgmlin)

#ALLSGML := $(wildcard $(srcdir)/*.sgml $(srcdir)/ref/*.sgml) $(GENERATED_SGML)
ALLSGML := $(ALLSGMLIN:%sgmlin=%sgml) $(GENERATED_SGML)

ALLSGMLTOREMOVE := $(ALLSGMLIN:%sgmlin=%sgml)


# Sometimes we don't want this one.
ALMOSTALLSGML := $(filter-out %bookindex.sgml,$(ALLSGML))

ifdef DOCBOOKSTYLE
CATALOG = -c $(DOCBOOKSTYLE)/catalog
endif

# Enable some extra warnings
# -wfully-tagged needed to throw a warning on missing tags
# for older tool chains, 2007-08-31
# Note: try "make SPFLAGS=-wxml" to catch a lot of other dubious constructs,
# in particular < and & that haven't been made into entities.  It's far too
# noisy to turn on by default, unfortunately.
override SPFLAGS += -wall -wno-unused-param -wno-empty -wfully-tagged

##
## SGML files
##

sgml-files: $(ALLSGMLIN:%sgmlin=%sgml)

##
## Man pages
##

man distprep-man: man-stamp

man-stamp: stylesheet-man.xsl postgres.xml
	$(XSLTPROC) $(XSLTPROCFLAGS) $(XSLTPROC_MAN_FLAGS) $^
	touch $@


##
## HTML
##

.PHONY: draft

JADE.html.call = $(JADE) $(JADEFLAGS) $(SPFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t sgml -i output-html

# The draft target creates HTML output in draft mode, without index (for faster build).
draft: postgres.sgml $(ALMOSTALLSGML) stylesheet.dsl
	$(MKDIR_P) html
	$(JADE.html.call) -V draft-mode $<
	cp $(srcdir)/stylesheet.css html/

html: html-stamp

html-stamp: postgres.sgml $(ALLSGML) stylesheet.dsl
	$(MKDIR_P) html
	$(JADE.html.call) -i include-index $<
	cp $(srcdir)/stylesheet.css html/
	touch $@

# single-page HTML
postgres.html: postgres.sgml $(ALLSGML) stylesheet.dsl
	$(JADE.html.call) -V nochunks -V rootchunk -V '(define %root-filename% #f)' -V '(define use-output-dir #f)' -i include-index $<

# single-page text
postgres.txt: postgres.html
	$(LYNX) -force_html -dump -nolist $< > $@

HTML.index: postgres.sgml $(ALMOSTALLSGML) stylesheet.dsl
	@$(MKDIR_P) html
	$(JADE.html.call) -V html-index $<

bookindex.sgml: HTML.index
	LC_ALL=C $(PERL) $(COLLATEINDEX) -f -g -i 'bookindex' -o $@ $<

# Technically, this should depend on Makefile.global, but then
# version.sgml would need to be rebuilt after every configure run,
# even in distribution tarballs.  So this is cheating a bit, but it
# will achieve the goal of updating the version number when it
# changes.
version.sgml: $(top_srcdir)/configure
	{ \
	  echo "<!ENTITY version \"$(VERSION)\">"; \
	  echo "<!ENTITY majorversion \"$(MAJORVERSION)\">"; \
	} > $@

features-supported.sgml: $(top_srcdir)/src/backend/catalog/sql_feature_packages.txt $(top_srcdir)/src/backend/catalog/sql_features.txt
	$(PERL) $(srcdir)/mk_feature_tables.pl YES $^ > $@

features-unsupported.sgml: $(top_srcdir)/src/backend/catalog/sql_feature_packages.txt $(top_srcdir)/src/backend/catalog/sql_features.txt
	$(PERL) $(srcdir)/mk_feature_tables.pl NO $^ > $@

errcodes-table.sgml: $(top_srcdir)/src/backend/utils/errcodes.txt generate-errcodes-table.pl
	$(PERL) $(srcdir)/generate-errcodes-table.pl $< > $@

##
## Print
##


# RTF to allow minor editing for hardcopy
%.rtf: %.sgml $(ALLSGML)
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t rtf -V rtf-backend -i output-print  -i include-index postgres.sgml

# TeX
# Regular TeX and pdfTeX have slightly differing requirements, so we
# need to distinguish the path we're taking.

JADE.tex.call = $(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d $(srcdir)/stylesheet.dsl -t tex -V tex-backend -i output-print -i include-index

%-A4.tex-ps: %.sgml $(ALLSGML)
	$(JADE.tex.call) -V texdvi-output -V '%paper-type%'=A4 -o $@ $<

%-US.tex-ps: %.sgml $(ALLSGML)
	$(JADE.tex.call) -V texdvi-output -V '%paper-type%'=USletter -o $@ $<

%-A4.tex-pdf: %.sgml $(ALLSGML)
	$(JADE.tex.call) -V texpdf-output -V '%paper-type%'=A4 -o $@ $<

%-US.tex-pdf: %.sgml $(ALLSGML)
	$(JADE.tex.call) -V texpdf-output -V '%paper-type%'=USletter -o $@ $<

%.dvi: %.tex-ps
	@rm -f $*.aux $*.log
# multiple runs are necessary to create proper intra-document links
	jadetex $<
	jadetex $<
	jadetex $<

# PostScript from TeX
postgres.ps:
	$(error Invalid target;  use postgres-A4.ps or postgres-US.ps as targets)

%.ps: %.dvi
	dvips -o $@ $<

postgres.pdf:
	$(error Invalid target;  use postgres-A4.pdf or postgres-US.pdf as targets)

%.pdf: %.tex-pdf
	@rm -f $*.aux $*.log $*.out
# multiple runs are necessary to create proper intra-document links
	pdfjadetex $<
	pdfjadetex $<
	pdfjadetex $<

# Cancel built-in suffix rules, interfering with PS building
.SUFFIXES:
.SUFFIXES: .sgml .sgmlin

INC_LIST = -I PG -I EN
#INC_LIST = -I PGXC -I EN

.sgmlin.sgml:
	$(MAKE) -C $(MAKESGMLDIR)
	$(MAKESGML) -i $< -o $@ $(INC_LIST) $(EXC_LIST)


# This generates an XML version of the flow-object tree.  It's useful
# for debugging DSSSL code, and possibly to interface to some other
# tools that can make use of this.
%.fot: %.sgml $(ALLSGML)
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t fot -i output-print -i include-index -o $@ $<


##
## Semi-automatic generation of some text files.
##

JADE.text = $(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -i output-text -t sgml
LYNX = lynx

INSTALL HISTORY regress_README: % : %.html
	$(PERL) -p -e 's/<H(1|2)$$/<H\1 align=center/g' $< | $(LYNX) -force_html -dump -nolist -stdin > $@

INSTALL.html: standalone-install.sgml installation.sgml version.sgml
	$(JADE.text) -V nochunks standalone-install.sgml installation.sgml > $@

HISTORY.html: generate_history.pl $(wildcard $(srcdir)/release*.sgml)
	$(PERL) $< "$(srcdir)" release.sgml >tempfile_HISTORY.sgml
	$(JADE.text) -V nochunks tempfile_HISTORY.sgml > $@
	rm tempfile_HISTORY.sgml

regress_README.html: regress.sgml
	( echo '<!DOCTYPE chapter PUBLIC "-//OASIS//DTD DocBook V4.2//EN" ['; \
	  echo '<!ENTITY % standalone-ignore "IGNORE">'; \
	  echo '<!ENTITY % standalone-include "INCLUDE"> ]>'; \
	  cat $< ) >tempfile_regress_README.sgml
	$(JADE.text) -V nochunks tempfile_regress_README.sgml > $@
	rm tempfile_regress_README.sgml


##
## XSLT processing
##

# For obscure reasons, gmake 3.81 complains about circular dependencies
# if we try to do "make all" in a VPATH build without the explicit
# $(srcdir) on the postgres.sgml dependency in this rule.  gmake bug?
postgres.xml: $(srcdir)/postgres.sgml $(ALMOSTALLSGML)
	$(OSX) -D. -x lower $< >postgres.xmltmp
	$(PERL) -p -e 's/\[(amp|copy|egrave|gt|lt|mdash|nbsp|ouml|pi|quot|uuml) *\]/\&\1;/g;' \
	           -e '$$_ .= qq{<!DOCTYPE book PUBLIC "-//OASIS//DTD DocBook XML V4.2//EN" "http://www.oasis-open.org/docbook/xml/4.2/docbookx.dtd">\n} if $$. == 1;' \
	  <postgres.xmltmp > $@
	rm postgres.xmltmp
# ' hello Emacs

xslthtml: stylesheet.xsl postgres.xml
	$(XSLTPROC) $(XSLTPROCFLAGS) $(XSLTPROC_HTML_FLAGS) $^

htmlhelp: stylesheet-hh.xsl postgres.xml
	$(XSLTPROC) $(XSLTPROCFLAGS) $^

%-A4.fo: stylesheet-fo.xsl %.xml
	$(XSLTPROC) $(XSLTPROCFLAGS) --stringparam paper.type A4 -o $@ $^

%-US.fo: stylesheet-fo.xsl %.xml
	$(XSLTPROC) $(XSLTPROCFLAGS) --stringparam paper.type USletter -o $@ $^


##
## Experimental Texinfo targets
##

DB2X_TEXIXML = db2x_texixml
DB2X_XSLTPROC = db2x_xsltproc
MAKEINFO = makeinfo

%.texixml: %.xml
	$(DB2X_XSLTPROC) -s texi -g output-file=$(basename $@) $< -o $@

%.texi: %.texixml
	$(DB2X_TEXIXML) --encoding=iso-8859-1//TRANSLIT $< --to-stdout > $@

%.info: %.texi
	$(MAKEINFO) --enable-encoding --no-split --no-validate $< -o $@


##
## Check
##

# Quick syntax check without style processing
check maintainer-check: postgres.sgml $(ALMOSTALLSGML) check-tabs
	$(NSGMLS) $(SPFLAGS) $(SGMLINCLUDE) -s $<


##
## Install
##

install: install-html

ifneq ($(PORTNAME), sco)
install: install-man
endif

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(htmldir)'/html $(addprefix '$(DESTDIR)$(mandir)'/man, 1 3 $(sqlmansectnum))

uninstall:
	rm -f '$(DESTDIR)$(htmldir)/html/'* $(addprefix  '$(DESTDIR)$(mandir)'/man, 1/* 3/* $(sqlmansectnum)/*)


## Install html

install-html: html installdirs
	cp -R $(call vpathsearch,html) '$(DESTDIR)$(htmldir)'


## Install man

install-man: man installdirs

sqlmansect ?= 7
sqlmansectnum = $(shell expr X'$(sqlmansect)' : X'\([0-9]\)')

# Before we install the man pages, we massage the section numbers to
# follow the local conventions.
#
ifeq ($(sqlmansectnum),7)
install-man:
	cp -R $(foreach dir,man1 man3 man7,$(call vpathsearch,$(dir))) '$(DESTDIR)$(mandir)'

else # sqlmansectnum != 7
fix_sqlmansectnum = sed -e '/^\.TH/s/"7"/"$(sqlmansect)"/' \
			-e 's/\\fR(7)/\\fR($(sqlmansectnum))/g' \
			-e '1s/^\.so man7/.so man$(sqlmansectnum)/g;1s/^\(\.so.*\)\.7$$/\1.$(sqlmansect)/g'

man: fixed-man-stamp

fixed-man-stamp: man-stamp
	@$(MKDIR_P) $(addprefix fixedman/,man1 man3 man$(sqlmansectnum))
	for file in $(call vpathsearch,man1)/*.1; do $(fix_sqlmansectnum) $$file >fixedman/man1/`basename $$file` || exit; done
	for file in $(call vpathsearch,man3)/*.3; do $(fix_sqlmansectnum) $$file >fixedman/man3/`basename $$file` || exit; done
	for file in $(call vpathsearch,man7)/*.7; do $(fix_sqlmansectnum) $$file >fixedman/man$(sqlmansectnum)/`basename $$file | sed s/\.7$$/.$(sqlmansect)/` || exit; done

install-man:
	cp -R $(foreach dir,man1 man3 man$(sqlmansectnum),fixedman/$(dir)) '$(DESTDIR)$(mandir)'

clean: clean-man clean-sgml

.PHONY: clean-man
clean-man:
	rm -rf fixedman/ fixed-man-stamp

.PHONY: clean-sgml
clean-sgml:
	rm -rf $(ALLSGML)

endif # sqlmansectnum != 7

# tabs are harmless, but it is best to avoid them in SGML files
check-tabs:
	@( ! grep '	' $(wildcard $(srcdir)/*.sgml $(srcdir)/ref/*.sgml) ) || (echo "Tabs appear in SGML files";  exit 1)

##
## Clean
##

# This allows removing some files from the distribution tarballs while
# keeping the dependencies satisfied.
.SECONDARY: postgres.xml $(GENERATED_SGML) HTML.index
.SECONDARY: INSTALL.html HISTORY.html regress_README.html
.SECONDARY: %-A4.tex-ps %-US.tex-ps %-A4.tex-pdf %-US.tex-pdf

clean:
# text --- these are shipped, but not in this directory
	rm -f INSTALL HISTORY regress_README
	rm -f INSTALL.html HISTORY.html regress_README.html
	rm -f ./ref/*.sgml
	touch $(top_builddir)/src/bin/psql/create_help.pl
# single-page output
	rm -f postgres.html postgres.txt
# print	rm -f *.rtf *.tex-ps *.tex-pdf *.dvi *.aux *.log *.ps *.pdf *.out *.fot
# index
	rm -f HTML.index $(GENERATED_SGML)
# XSLT
	rm -f postgres.xml postgres.xmltmp htmlhelp.hhp toc.hhc index.hhk *.fo
# Texinfo
	rm -f *.texixml *.texi *.info db2texi.refs

distclean: clean

maintainer-clean: distclean
# HTML
	rm -fr html/ html-stamp
# man
	rm -rf man1/ man3/ man7/ man-stamp
# sgml
	rm -f $(ALLSGMLTOREMOVE)

.PHONY: sgmlfiles

INC_LIST = -I XC -I EN
EXC_LIST = -E PG -E JP

sgmlfiles:
	./makesgmlfiles $(INC_LIST) $(EXC_LIST)
