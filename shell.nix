{ pkgs ? import <nixpkgs> {} }:

with pkgs;

mkShell {
  buildInputs = [
    cmake
    pkg-config
    # git
    libgit2
    # libgit2.lib
    # libgit2.dev

    # for libgit2
    openssl
    pcre2
    zlib
  ];
}
