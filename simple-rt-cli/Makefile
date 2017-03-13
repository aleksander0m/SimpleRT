# SimpleRT: Reverse tethering utility for Android
# Copyright (C) 2016 Konstantin Menyaev
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

TARGET = g-simple-rt
LDFLAGS = -lm -lpthread
CC = gcc
CFLAGS = -g -std=c99 -D_DEFAULT_SOURCE -Wall

CFLAGS += `pkg-config --cflags libusb-1.0 glib-2.0 gudev-1.0`
LDFLAGS += `pkg-config --libs libusb-1.0 glib-2.0 gudev-1.0`

.PHONY: default all clean

default: $(TARGET)
all: default

CSRC = g-simple-rt.c
COBJ = $(CSRC:.c=.o)

$(COBJ): $(CSRC)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(COBJ)
	$(CC) $(COBJ) -Wall $(LDFLAGS) -o $@

clean:
	-rm -f $(COBJ)
	-rm -f $(TARGET)
