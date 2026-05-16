import configparser
import subprocess
import os
from os.path import abspath, dirname, join

run_number = os.getenv('GITHUB_RUN_NUMBER', '0')
build_location = os.getenv('BUILD_LOCATION', 'local')


def _short_from_override(override: str) -> str:
    parts = override.split(".")
    nums = []
    for p in parts:
        if p.isdigit():
            nums.append(p)
            if len(nums) == 3:
                return ".".join(nums)
        elif nums:
            break
    return override.split("-", 1)[0] if "-" in override else override


def readProps(prefsLoc):
    """Read the version of our project as a string"""

    override = os.getenv("MESHTASTIC_APP_VERSION_OVERRIDE", "").strip()
    if not override:
        ci_path = join(dirname(abspath(prefsLoc)), ".ci-app-version")
        try:
            with open(ci_path, encoding="utf-8") as f:
                override = f.read().splitlines()[0].strip()
        except (OSError, IndexError):
            override = ""
    if override:
        short = _short_from_override(override)
        verObj = dict(short=short, long=override, deb="unset")
        try:
            sha = (
                subprocess.check_output(["git", "rev-parse", "--short=7", "HEAD"])
                .decode("utf-8")
                .strip()
            )
            verObj["deb"] = "{}.{}~{}{}".format(
                verObj["short"], run_number, build_location, sha
            )
        except Exception:
            verObj["deb"] = "{}.{}~{}".format(
                verObj["short"], run_number, build_location
            )
        return verObj

    config = configparser.RawConfigParser()
    config.read(prefsLoc)
    version = dict(config.items("VERSION"))
    verObj = dict(
        short="{}.{}.{}".format(version["major"], version["minor"], version["build"]),
        long="unset",
        deb="unset",
    )

    # Try to find current build SHA if if the workspace is clean.  This could fail if git is not installed
    try:
        # Pin abbreviation length to keep local builds and CI matching (avoid auto-shortening)
        sha = (
            subprocess.check_output(["git", "rev-parse", "--short=7", "HEAD"])
            .decode("utf-8")
            .strip()
        )
        isDirty = (
            subprocess.check_output(["git", "diff", "HEAD"]).decode("utf-8").strip()
        )
        suffix = sha
        # if isDirty:
        #     # short for 'dirty', we want to keep our verstrings source for protobuf reasons
        #     suffix = sha + "-d"
        verObj["long"] = "{}.{}".format(verObj["short"], suffix)
        verObj["deb"] = "{}.{}~{}{}".format(verObj["short"], run_number, build_location, sha)
    except:
        # print("Unexpected error:", sys.exc_info()[0])
        # traceback.print_exc()
        verObj["long"] = verObj["short"]
        verObj["deb"] = "{}.{}~{}".format(verObj["short"], run_number, build_location)

    # print("firmware version " + verStr)
    return verObj


# print("path is" + ','.join(sys.path))