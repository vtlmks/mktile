# mktile

A simple X11 window tiler. Select windows on your current workspace and tile them vertically, horizontally, or in a configurable grid.

## Dependencies

- C99 compiler
- FreeType2
- libX11
- libXext
- libm (math)

## Clone

```sh
git clone --recurse-submodules https://github.com/vtlmks/mktile.git
```

If you already cloned without submodules:

```sh
git submodule update --init --recursive
```

## Build

```sh
./build.sh
```

## Usage

Run `./mktile`, check the windows you want to tile, and click one of:

- **Tile V** — stack windows top to bottom
- **Tile H** — stack windows left to right
- **Tile Grid** — arrange in a rows x cols grid (configurable with spinbuttons)

The program exits after tiling.

## License

MIT
