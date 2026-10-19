# midscroll-for-x11
A middle-click-scroll like on windows but for x11 (made using AI)
---
# Requirements
An X11 session (test it via running in terminal `echo $XDG_SESSION_TYPE` which should return `x11`)
---
# Usage
To use this just download the executable and run it in terminal :)
---
# Build it yourself
To build this first install the dependencies
```bash
# Debian / Ubuntu / Mint
sudo apt install build-essential pkg-config libx11-dev libxtst-dev libxext-dev libatspi2.0-dev libglib2.0-dev

# Fedora
sudo dnf install gcc pkgconf-pkg-config libX11-devel libXtst-devel libXext-devel at-spi2-core-devel glib2-devel

# Arch
sudo pacman -S base-devel libx11 libxtst libxext at-spi2-core glib2
```
Then build and install the code
```bash
gcc -O2 -o midscroll midscroll.c $(pkg-config --cflags --libs atspi-2 glib-2.0 gobject-2.0) -lX11 -lXtst -lXext
install -Dm755 midscroll ~/.local/bin/midscroll
gsettings set org.gnome.desktop.interface toolkit-accessibility true
```
Last one is needed so scroll won't activate on title bars n stuff btw
---
# Configuration
Config should be located in `~/.config/midscroll.conf`
Here's the base config
```
speed = 0.25
deadzone = 12
exclude = kitty, Alacritty
ignore_top = google-chrome:100
unknown = scroll
```
And a startup shortcut should be made in `~/.config/autostart/midscroll.desktop`
Here's the code for that
```
[Desktop Entry]
Type=Application
Name=midscroll
Exec=sh -c "\$HOME/.local/bin/midscroll"
NoDisplay=true
```
