#!/usr/bin/env bash
OBJCC=${OBJCC:-clang}

# To debug a build failure, uncomment this line:
# set -x

HERE="$(dirname "${0}")"
ODIR="${ODIR:-"${HERE}"/output}"
[ -d "${ODIR}" ] || (mkdir -p "${ODIR}" || exit 1)

"${OBJCC}" -fsyntax-only "${HERE}"/mu_macos_unit.m -DMU_MACOS_RUN_MODE=MU_MACOS_RUN_MODE_PLAIN -Wall -Werror || exit 1
"${OBJCC}" -fsyntax-only "${HERE}"/mu_macos_unit.m -DMU_MACOS_RUN_MODE=MU_MACOS_RUN_MODE_COROUTINE -Wall -Werror || exit 1
(O="${ODIR}"/mu_test.elf ;
 "${OBJCC}" -o "${O}" \
	    -DMU_MACOS_RUN_MODE=MU_MACOS_RUN_MODE_COROUTINE \
	    "${HERE}"/mu_macos_unit.m \
	    "${HERE}"/mu_test_unit.c \
	    -Wall \
	    -framework OpenGL \
	    -framework IOKit \
	    -framework AppKit \
	    -framework CoreAudio \
	    -g -O2 \
	    -std=c11 \
    && printf "PROGRAM\t%s\n" "${O}") || exit 1

# Bundle app:
(O="${ODIR}"/mutest.app O_ELFNAME="mu_test.elf" I_ELF="${ODIR}/mu_test.elf" I_PLIST="${HERE}/mu_test-macos-Info.plist"
 [ -f "${I_ELF}" ] &&
 [ -f "${I_PLIST}" ] &&
 rm -rf "${O}"  &&
 mkdir -p "${O}"/Contents/MacOS &&
 cp "${I_PLIST}" "${O}"/Contents/Info.plist &&
 cp "${I_ELF}" "${O}"/Contents/MacOS/"${O_ELFNAME}" &&
 printf "MACOS_APP\t%s\n" "${O}") || exit 1
