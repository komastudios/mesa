#!/bin/bash

generate_global_rootfs_tag()
{
    md5sum <(printenv | grep "ROOTFS_.*_TAG") |
        head -c 32
}

ROOTFS_TAG=$(generate_global_rootfs_tag)
export ROOTFS_TAG

# Useful variables for local setup used during testing
# MINIO_HOST="localhost:9000"
# MINIO_PROTOCOL="http"
# MINIO_LOGIN_ENDPOINT="${MINIO_PROTOCOL}://${MINIO_HOST}"
# CHROOT_CMD="fakechroot fakeroot chroot"
# DEBOOTSTRAP_CMD="fakechroot fakeroot debootstrap"

export CHROOT_CMD="${CHROOT_CMD:-chroot}"
export DEBOOTSTRAP_CMD="${DEBOOTSTRAP_CMD:-debootstrap}"
export MINIO_PROTOCOL=${MINIO_PROTOCOL:-https}
export DEBIAN_DIR=debian_${DEBIAN_BUILD_TAG}/${DEBIAN_ARCH}
export MINIO_ROOTFS_DIR=${MINIO_HOST}/mesa-lava/${CI_PROJECT_PATH}/${DISTRIBUTION_TAG}/${DEBIAN_ARCH}
export MINIO_DEPS_DIR=${MINIO_HOST}/mesa-lava/${CI_PROJECT_PATH}/${DEBIAN_DIR}
# Built artifacts could also be located at mainline folder
export MAINLINE_MINIO_DEPS_DIR=${MINIO_HOST}/mesa-lava/${FDO_UPSTREAM_REPO}/${DEBIAN_DIR}
export MINIO_ROOTFS_UPLOAD_DIR="minio://${MINIO_ROOTFS_DIR}"
export MINIO_DEPS_UPLOAD_DIR="minio://${MINIO_DEPS_DIR}"
export TMP_ROOTFS_DIR=/tmp/rootfs
export DONE_FILE=${ROOTFS_TAG}.done
export ROOTFS_FILE=lava-rootfs_${ROOTFS_TAG}.tgz

check_minio()
{
    # MINIO_FOLDER="${MINIO_HOST}/mesa-lava/$1/${DISTRIBUTION_TAG}/${DEBIAN_ARCH}"
    # if wget -q --method=HEAD "https://${MINIO_FOLDER}/done"; then
    #     exit
    # fi
    if is_dep_available_in_minio "${DONE_FILE}"; then
        exit
    else
        echo "rootfs file needs to be recreated."
    fi
}

login_to_minio()
(
    # set +x
    # ci-fairy minio login stores credentials at .minio_credentials file.
    # No need to login again if this files exists and the credentials are not expired.
    [ -f .minio_credentials ] && (
        set +x
        EXPIRE_DATE=$(grep -Po '"\d\d\d\d-\d\d-\d\dT\d\d:\d\d:\d\d\+\d\d:\d\d"' .minio_credentials | tr -d '"')
        EXPIRE_TS=$(date -d "${EXPIRE_DATE}" +%s)
        NOW_TS=$(date +%s)

        [ "$EXPIRE_TS" -ge "$NOW_TS" ]
    ) && return 0

    ENDPOINT=${MINIO_LOGIN_ENDPOINT:+--endpoint-url ${MINIO_LOGIN_ENDPOINT}}
    ci-fairy minio login ${ENDPOINT} --token-file "${CI_JOB_JWT_FILE}"
)

# Args:
# $1: remote filepath
is_dep_available_in_minio()
(
    # set +x
    login_to_minio
    MINIO_PATH="${MINIO_UPLOAD_DIR}/${1}"
    # Check both user and project directories
    if ci-fairy minio ls "${MINIO_PATH}" || ci-fairy minio ls "${MAINLINE_MINIO_DEPS_DIR}"; then
        return 0
    fi

    # Package with this version not found
    return 1
)

# Args:
# $1: minio relative path
# $2-*: local file paths
upload_to_minio()
(
    set +x
    login_to_minio || return 1
    MINIO_PATH="${MINIO_UPLOAD_DIR}/${1}"
    shift 1
    FILES_TO_UPLOAD="${*}"

    for f in $FILES_TO_UPLOAD; do
        set -x
        ci-fairy minio cp "${f}" \
                "${MINIO_PATH}/$f"
        set +x
    done
)

# Args:
# $1: local path
# $2-*: remote relative file paths
download_from_minio()
(
    set +x
    login_to_minio || return 1
    LOCAL_PATH="${1}"
    shift 1
    FILES_TO_DOWNLOAD="${*}"
    for f in $FILES_TO_DOWNLOAD; do
        set -x
        ci-fairy minio cp "${MINIO_UPLOAD_DIR}/$f" \
            "${LOCAL_PATH}"
        set +x
    done
)

resolve_file_from_dep_name()
{
    set +x
    export DEPENDENCY_NAME=${1}
    export DEPENDENCY_RESOLUTION=ROOTFS_${DEPENDENCY_NAME^^}_TAG
    export DEPENDENCY_TAG=${!DEPENDENCY_RESOLUTION}
    : "${DEPENDENCY_TAG:?Could not find \$${DEPENDENCY_RESOLUTION}, please set image_tags.yml properly.}"
    export DEPENDENCY_FILE=${DEPENDENCY_NAME}_${DEPENDENCY_TAG}.tar.xz
    set -x
}

# Args
# 1: Dependency name
# 2: Target directory of the tarball
artifact_repo_sync()
{
    set +x
    mkdir -p ${TMP_ROOTFS_DIR}
    resolve_file_from_dep_name "${1}"
    ROOTFS_INSTALL_DIR=${2:-/lava-files/rootfs-${DEBIAN_ARCH}}
    MINIO_UPLOAD_DIR=${MINIO_DEPS_UPLOAD_DIR}

    if is_dep_available_in_minio deps/"${DEPENDENCY_FILE}"
    then
        echo "Dependency ${DEPENDENCY_NAME} for tag ${DEPENDENCY_TAG} is found on server."
        echo "Downloading ${DEPENDENCY_FILE}"
        download_from_minio "${INSTALL_DIR}" deps/"${DEPENDENCY_FILE}"
    else
        echo -e "\e[0Ksection_start:$(date +%s):build_$1[collapsed=true]\r\e[0KBuilding $1..."
        set -x
        "build_$1"
        set +x
        echo -e "\e[0Ksection_end:$(date +%s):build_$1\r\e[0K"
        mkdir -p "${ROOTFS_INSTALL_DIR}"
        tar -cJvf "${DEPENDENCY_FILE}" -C "${TMP_ROOTFS_DIR}" .
        upload_to_minio deps "${DEPENDENCY_FILE}"
    fi

    tar xf "${DEPENDENCY_FILE}" -C "$ROOTFS_INSTALL_DIR"
    rm -Rf "${DEPENDENCY_FILE}" "${TMP_ROOTFS_DIR:?}"/*
    set -x
}
