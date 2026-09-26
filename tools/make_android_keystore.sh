#!/bin/sh
# Creates this project's own release signing key for the Android app and writes
# android/keystore.properties, which the Gradle build reads.
#
# Why this exists: android/app/build.gradle refuses to sign a release with the
# shared debug key unless that is asked for explicitly. The debug key is not an
# identity - it is public, it is the same everywhere, and a package signed with
# it has to be uninstalled before a properly signed one will install, because
# Android treats a signature change as a different app.
#
# The keystore is generated here and deliberately not added to the repository.
# Losing it means a new application id, so back it up somewhere the build
# machine is not.
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
android_dir="$root_dir/android"
store_name=${MP_APK_KEYSTORE_NAME:-metroid-prime-port-release.jks}
alias_name=${MP_APK_KEY_ALIAS:-metroid-prime-port}
store_file="$android_dir/$store_name"
props_file="$android_dir/keystore.properties"
distinguished_name=${MP_APK_KEY_DN:-"CN=Metroid Prime Port, OU=Port, O=Metroid Prime Port, L=, ST=, C="}
validity_days=${MP_APK_KEY_VALIDITY:-10950}   # 30 years: Android rejects an
                                             # expired signing key outright.

keytool=""
for candidate in \
    "${JAVA_HOME:-}/bin/keytool" \
    /usr/lib/jvm/*/bin/keytool \
    "$(command -v keytool 2>/dev/null || true)"
do
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then
        keytool=$candidate
        break
    fi
done
if [ -z "$keytool" ]; then
    echo "error: keytool not found; install a JDK or set JAVA_HOME" >&2
    exit 1
fi

if [ -f "$store_file" ]; then
    echo "error: $store_file already exists; refusing to overwrite an existing identity" >&2
    echo "       delete it only if you are certain it is no longer needed: an app" >&2
    echo "       signed with it cannot be updated by one signed with another key" >&2
    exit 1
fi

# Read the passwords without echoing them, and without putting them in the
# process list where another user on the machine could see them.
if [ -t 0 ]; then
    printf 'Password for the new key (and for the key inside it): ' >&2
    stty -echo 2>/dev/null || true
    read -r passwords_match || true
    stty echo 2>/dev/null || true
    printf '\n' >&2
    if [ -z "$passwords_match" ]; then
        echo >&2
        echo "error: empty password; a signing key needs one" >&2
        exit 1
    fi
    password=$passwords_match
else
    password=${MP_APK_KEYSTORE_PASSWORD:-}
    if [ -z "$password" ]; then
        echo "error: no terminal to prompt on; set MP_APK_KEYSTORE_PASSWORD" >&2
        exit 1
    fi
fi

echo "creating $store_file (alias $alias_name, $validity_days days)"
"$keytool" -genkeypair \
    -keystore "$store_file" \
    -storetype PKCS12 \
    -storepass "$password" \
    -keypass "$password" \
    -alias "$alias_name" \
    -keyalg RSA \
    -keysize 4096 \
    -validity "$validity_days" \
    -dname "$distinguished_name" >/dev/null

cat > "$props_file" <<EOF
# Written by tools/make_android_keystore.sh. Not tracked: this file names a
# private key. Keep $store_name and this file somewhere safe - an app signed
# with one key cannot be updated by one signed with another.
storeFile=$store_name
storePassword=$password
keyAlias=$alias_name
keyPassword=$password
EOF
chmod 600 "$props_file"

echo "wrote $props_file"
echo
echo "Next: tools/android_apk.sh :app:assembleRelease"
echo "Back up both files. Losing them means a new application id."
