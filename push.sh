#!/bin/bash
git add .
git commit -m "${1:-update: $(date '+%Y-%m-%d %H:%M')}"
git push origin main