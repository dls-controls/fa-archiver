# Install files in global directory

ifndef PYTHON
PYTHON = python
endif

# Install a lightly edited wrapper to run fa-viewer.
FA_SUBST = \
    's:^PYTHON=.*$$:PYTHON='\'$(PYTHON)\'':; \
     s:^HERE=.*$$:HERE='\'$(CURDIR)\'':'

all:

clean:
	rm -f *.pyc

install:
ifndef SCRIPT_DIR
	echo >&2 Must define SCRIPT_DIR; false
endif
	$(PYTHON) -m compileall .
	sed <fa-viewer >$(SCRIPT_DIR)/fa-viewer $(FA_SUBST)
	chmod +x $(SCRIPT_DIR)/fa-viewer

.PHONY: all clean install