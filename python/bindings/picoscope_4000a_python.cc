/*
 * Copyright 2021 Free Software Foundation, Inc.
 *
 * This file is part of GNU Radio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/***********************************************************************************/
/* This file is automatically generated using bindtool and can be manually edited  */
/* The following lines can be configured to regenerate this file during cmake      */
/* If manual edits are made, the following tags should be modified accordingly.    */
/* BINDTOOL_GEN_AUTOMATIC(0)                                                       */
/* BINDTOOL_USE_PYGCCXML(0)                                                        */
/* BINDTOOL_HEADER_FILE(picoscope_4000a.h)                                        */
/* BINDTOOL_HEADER_FILE_HASH(96422d0062f15ad8c673ce604de4645e)                     */
/***********************************************************************************/

#include <pybind11/complex.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

#include <digitizers_39/picoscope_4000a.h>
// pydoc.h is automatically generated in the build directory
#include <picoscope_4000a_pydoc.h>

void bind_picoscope_4000a(py::module& m)
{

    using picoscope_4000a = ::gr::digitizers_39::picoscope_4000a;


    py::class_<picoscope_4000a, gr::digitizers_39::digitizer_block, gr::sync_block, gr::block, gr::basic_block,
        std::shared_ptr<picoscope_4000a>>(m, "picoscope_4000a", D(picoscope_4000a))

        .def(py::init(&picoscope_4000a::make),
           py::arg("serial_number"),
           py::arg("auto_arm"),
           D(picoscope_4000a,make)
        )
        



        ;




}








