all::
RSYNC_DEST := bogomips.org:/srv/bogomips/sleepy_penguin
rfproject := rainbows
rfpackage := sleepy_penguin
include pkg.mk
ifneq ($(VERSION),)
release::
	$(RAKE) raa_update VERSION=$(VERSION)
	$(RAKE) publish_news VERSION=$(VERSION)
endif
.PHONY: .FORCE-GIT-VERSION-FILE doc test $(test_units) manifest
