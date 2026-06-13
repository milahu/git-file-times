#!/bin/sh

# https://serverfault.com/questions/401437
# How to retrieve the last modification date of all files in a Git repository

# git ls-files -z | xargs -0 -I{} -- git log -1 --format="%ct {}" {}

# git log --follow: follow renames of files, which can give a different result
git ls-files -z | xargs -0 -I{} -- git log -1 --follow --format="%ct {}" {}
