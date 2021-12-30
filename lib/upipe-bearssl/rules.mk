configs += bearssl
bearssl-includes = bearssl.h
bearssl-ldlibs = -lbearssl

lib-targets = libupipe_bearssl

libupipe_bearssl-desc = TLS modules

libupipe_bearssl-src = \
    https_source_hook.c \
    https_source_hook.h \
    uprobe_https.c

libupipe_bearssl-includes = uprobe_https.h
libupipe_bearssl-libs = libupipe libupipe_modules bearssl
libupipe_bearssl-ldlibs = -lm
