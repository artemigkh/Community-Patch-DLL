"""Tap Scroll Lock the way a person does: key down, hold 250 ms, key up.

myth_watch.py polls GetAsyncKeyState at 20 Hz; a synthesized tap with no hold can fall between two
polls, so this holds it long enough to be seen. Tapping twice leaves the LED where it was.
"""
import ctypes
import time

VK_SCROLL, SCAN, KEYUP = 0x91, 0x46, 0x0002
user32 = ctypes.windll.user32
user32.keybd_event(VK_SCROLL, SCAN, 0, 0)
time.sleep(0.25)
user32.keybd_event(VK_SCROLL, SCAN, KEYUP, 0)
print("tapped Scroll Lock")
