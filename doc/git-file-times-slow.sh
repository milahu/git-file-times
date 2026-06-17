#!/bin/sh

# https://serverfault.com/questions/401437
# How to retrieve the last modification date of all files in a Git repository

# git ls-files -z | xargs -0 -I{} -- git log -1 --format="%ct {}" {}

# git log --follow --diff-filter=AM: do not count file renames as file modifications
# A = file was added
# C = file was copied
# M = file was modified
git ls-files -z | xargs -0 -I{} -- git log --follow --diff-filter=ACM -1 --format="%ct {}" {}
