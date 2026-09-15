#!/usr/bin/env python3
"""Punto d'ingresso: ./run.py build -i 'tinitaly/*.tif' -o dataset/italia"""
import sys
from geoworld.cli import main

if __name__ == "__main__":
    sys.exit(main())
