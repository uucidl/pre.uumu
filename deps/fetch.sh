#!/usr/bin/env bash
set -e
depfile="${1:?missing depfile}"
depname="$(basename "${depfile}" .txt)"

is_manual="$(sed -n -e 's/@manual *$/is_manual/p' "${depfile}")"
url="$(sed -n -e 's/@url: *//p' "${depfile}")"
name="$(sed -n -e 's/@name: *//p' "${depfile}")"
if [[ -z "${name}" ]] ; then name="${depname}" ; fi
if [[ "${is_manual}" == "is_manual" ]]; then
	curl -L "${url}" -O
	printf "WARN: Please extract this dependency manually.\n"
	exit 0
fi
package="${depname}.zip"
curl -L "${url}" -o "${package}"
depdir="./"${depname}""
[[ -d "${depdir}-unpacked" ]] && rm -rf "${depdir}-unpacked"
unzip "${package}" -d "${depdir}-unpacked"
[[ -d "${depdir}" ]] && rm -rf "${depdir}"
mkdir "${depdir}"
mv "${depdir}-unpacked"/"${name}"-*/* "${depdir}"
rm -rf "${depdir}-unpacked"/

