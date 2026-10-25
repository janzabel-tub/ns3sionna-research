"""Validate the packages required by the ns3sionna Python server."""

from importlib.metadata import PackageNotFoundError, version


EXPECTED_VERSIONS = {
    "sionna": "2.2.0",
    "sionna-rt": "2.2.0",
    "mitsuba": "3.9.1",
    "drjit": "1.5.0",
}


def installed_version(distribution):
    """Return a distribution version and fail with a useful package name."""
    try:
        return version(distribution)
    except PackageNotFoundError as error:
        raise RuntimeError("Package {} is not installed".format(distribution)) from error


for package, expected in EXPECTED_VERSIONS.items():
    actual = installed_version(package)
    if actual != expected:
        raise RuntimeError(
            "Expected {}=={}, found {}".format(package, expected, actual)
        )
    print("{} {} is installed".format(package, actual))

# Import Sionna RT first so that it selects the supported Mitsuba variant.
import sionna.rt as rt  # noqa: E402
import mitsuba as mi  # noqa: E402
import drjit as dr  # noqa: E402
import numpy as np  # noqa: E402
import torch  # noqa: E402
import zmq  # noqa: E402


print("Sionna RT module:", rt.__version__)
print("Mitsuba:", mi.__version__)
print("Dr.Jit:", dr.__version__)
print("Mitsuba variant:", mi.variant())
print("NumPy:", np.__version__)
print("PyTorch:", torch.__version__)
print("PyTorch CUDA available:", torch.cuda.is_available())
print("ZeroMQ:", zmq.zmq_version())
