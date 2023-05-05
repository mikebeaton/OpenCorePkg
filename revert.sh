#!/bin/bash
git branch -D revert
git checkout -b revert
git revert b07843fe1d47454747ae4eda9ea0189aa9fb8c03
git add Changelog.md
git revert --continue
