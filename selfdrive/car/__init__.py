from cereal import car
import os

from common.realtime import sec_since_boot
from common.fingerprints import eliminate_incompatible_cars, all_known_cars

from selfdrive.swaglog import cloudlog
import selfdrive.messaging as messaging
from .honda.interface import CarInterface as HondaInterface

try:
  from .toyota.interface import CarInterface as ToyotaInterface
except ImportError:
  ToyotaInterface = None

try:
  from .simulator.interface import CarInterface as SimInterface
except ImportError:
  SimInterface = None

try:
  from .simulator2.interface import CarInterface as Sim2Interface
except ImportError:
  Sim2Interface = None


interfaces = {
  "HONDA CIVIC 2016 TOURING": HondaInterface,
  "ACURA ILX 2016 ACURAWATCH PLUS": HondaInterface,
  "HONDA ACCORD 2016 TOURING": HondaInterface,
  "HONDA CR-V 2016 TOURING": HondaInterface,
  "TOYOTA PRIUS 2017": ToyotaInterface,
  "TOYOTA RAV4 2017": ToyotaInterface,

  "simulator": SimInterface,
  "simulator2": Sim2Interface
}

# **** for use live only ****
def fingerprint(logcan, timeout):
  if os.getenv("SIMULATOR") is not None or logcan is None:
    return ("simulator", None)
  elif os.getenv("SIMULATOR2") is not None:
    return ("simulator2", None)

  finger_st = sec_since_boot()

  cloudlog.warning("waiting for fingerprint...")
  candidate_cars = all_known_cars()
  finger = {}
  st = None
  while 1:
    for a in messaging.drain_sock(logcan, wait_for_one=True):
      if st is None:
        st = sec_since_boot()
      for can in a.can:
        if can.src == 0:
          finger[can.address] = len(can.dat)
        candidate_cars = eliminate_incompatible_cars(can, candidate_cars)

    ts = sec_since_boot()
    # if we only have one car choice and it's been 100ms since we got our first message, exit
    if len(candidate_cars) == 1 and st is not None and (ts-st) > 0.1:
      break
    # bail if no cars left or we've been waiting too long
    elif len(candidate_cars) == 0 or (timeout and ts-finger_st > timeout):
      return None, finger

  cloudlog.warning("fingerprinted %s", candidate_cars[0])
  return (candidate_cars[0], finger)


def get_car(logcan, sendcan=None, timeout=None):
  candidate, fingerprints = fingerprint(logcan, timeout)

  if candidate is None:
    cloudlog.warning("car doesn't match any fingerprints: %r", fingerprints)
    return None, None

  interface_cls = interfaces[candidate]
  params = interface_cls.get_params(candidate, fingerprints)

  return interface_cls(params, logcan, sendcan), params
