/* Copyright (c) 2026 gsf2wav contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

SECTIONS { .text 0x08000000 : { *(.text.start) *(.text*) } /DISCARD/ : { *(.ARM.exidx*) } }
