{ pkgs ? import <nixpkgs> {} }:

with pkgs;

mkShell {
  buildInputs = [
    cmake
    pkg-config
    # git
    # libgit2
    (libgit2.overrideAttrs (oldAttrs: {
      # NOTE this requires a patched version of libgit2
      # https://github.com/libgit2/libgit2/pull/7296
      # export all internal git functions
      src = ./src/vendor/libgit2;
      /*
      cmakeFlags = (oldAttrs.cmakeFlags or []) ++ [
        "-DBUILD_TESTS=OFF" # dont build tests
      ];
      doCheck = false; # dont run tests
      */
    }))
    # libgit2.lib
    # libgit2.dev

    # for libgit2
    openssl
    pcre2
    zlib
  ];
}
