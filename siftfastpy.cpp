// exact C++ implementation of lowe's sift program
// Copyright (C) zerofrog(@gmail.com), 2008-2009
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//Lesser GNU General Public License for more details.
//
//You should have received a copy of the GNU Lesser General Public License
//along with this program.  If not, see <http://www.gnu.org/licenses/>.
#define BOOST_ENABLE_ASSERT_HANDLER

#include <vector>
#include <boost/shared_ptr.hpp>

#define PY_ARRAY_UNIQUE_SYMBOL PyArrayHandle
#include <boost/python.hpp>
#include <boost/python/exception_translator.hpp>
#include <pyconfig.h>
#include <numpy/arrayobject.h>

#include <boost/format.hpp>

#include "siftfast.h"

#define CHECK_POINTER(p) { \
    if( (p) == NULL ) throw siftfast_exception("invalid pointer"); \
}

using namespace boost::python;
using namespace std;

// aligned malloc and free
inline void* py_aligned_malloc(size_t size, size_t align)
{
    assert( align <= 0xffffffff );
	char* p = (char*)malloc(size+align);
    if( p == NULL ) {
        fprintf(stderr,"py_aligned_malloc out of memory allocating %d bytes\n",(int)(size+align));
        return NULL;
    }
	int off = 4+align - ((int)(size_t)(p+4) % align);

	p += off;
	*(unsigned int*)(p-4) = off;

	return p;
}

void py_aligned_free(void* pmem)
{
    if( pmem != NULL ) {
        char* p = (char*)pmem;
        free(p - (int)*(unsigned int*)(p-4));
    }
}

struct siftfast_exception : std::exception
{
    siftfast_exception() : std::exception(), _s("unknown exception") {}
    siftfast_exception(const string& s) : std::exception() { _s = "siftfast: " + s; }
    virtual ~siftfast_exception() throw() {}
    char const* what() const throw() { return _s.c_str(); }
    string _s;
};

#if defined(BOOST_ENABLE_ASSERT_HANDLER)
namespace boost
{
inline void assertion_failed(char const * expr, char const * function, char const * file, long line)
{
    throw siftfast_exception(str(boost::format("[%s:%d] -> %s, expr: %s")%file%line%function%expr));
}
}
#endif

void translate_siftfast_exception(siftfast_exception const& e)
{
    // Use the Python 'C' API to set up an exception object
    PyErr_SetString(PyExc_RuntimeError, e.what());
}

inline object toPyArrayN(const float* pvalues, int N)
{
    npy_intp dims[] = {N};
    PyObject *pyvalues = PyArray_SimpleNew(1,dims, PyArray_FLOAT);
    if( pvalues != NULL )
        memcpy(PyArray_DATA(pyvalues),pvalues,N*sizeof(float));
    return static_cast<numeric::array>(handle<>(pyvalues));
}

template <typename T>
inline vector<T> ExtractArray(object o)
{
    vector<T> v(len(o));
    for(size_t i = 0; i < v.size(); ++i)
        v[i] = extract<T>(o[i]);
    return v;
}

inline vector<float> ExtractFloatArray(object oraw)
{
    object o = oraw.attr("flat");

    // check the types of o
    extract<float> xr(o[0]);
    if( xr.check() )
        return ExtractArray<float>(o);

    vector<float> v(len(o));
    object onew = ((numeric::array)oraw).astype("f8").attr("flat");
    for(size_t i = 0; i < v.size(); ++i)
        v[i] = (float)(extract<double>(onew[i]));
    return v;
}

class PyImage
{
public:
    PyImage(int width, int height) : width(width), height(height)
    {
        BOOST_ASSERT(width>0&&height>0);
        stride = (width+3)&~3;
        vimage.resize(height*stride);
    }
    PyImage(object oimage)
    {
        object shape = oimage.attr("shape");
        BOOST_ASSERT(len(shape)==2);
        width = extract<int>(shape[1]);
        height = extract<int>(shape[0]);
        stride = (width+3)&~3;
        vimage.resize(height*stride);
        SetData(oimage);
    }

    void SetData(object arr)
    {
        object shape = arr.attr("shape");
        if( len(shape) != 2 )
            throw siftfast_exception("array needs 2 dimensions");
        if( height != extract<int>(shape[0]) )
            throw siftfast_exception("array rows do not match height");
        if( width != extract<int>(shape[1]) )
            throw siftfast_exception("array columns do not match width");

        string dtype = extract<string>(arr.attr("dtype").attr("name"));
        if( (dtype.size() >= 3 && dtype[0] == 'i' && dtype[1] == 'n' && dtype[2] == 't') ||
            (dtype.size() >= 4 && dtype[1] == 'i' && dtype[2] == 'n' && dtype[3] == 't') ) {
            extract<int> xi(arr[0][0]);
            if( xi.check() ) {
                for(int i = 0; i < height; ++i)
                    for(int j = 0; j < width; ++j)
                        vimage[i*stride+j] = extract<int>(arr[i][j])*(1.0f/255.0f);
                return;
            }
        }

        extract<float> xr(arr[0][0]);
        if( xr.check() ) {
            for(int i = 0; i < height; ++i)
                for(int j = 0; j < width; ++j)
                    vimage[i*stride+j] = extract<float>(arr[i][j]);
            return;
        }
        extract<double> xd(arr[0][0]);
        if( xd.check() ) {
            for(int i = 0; i < height; ++i)
                for(int j = 0; j < width; ++j)
                    vimage[i*stride+j] = extract<double>(arr[i][j]);
            return;
        }

        throw siftfast_exception("array not in correct format");
    }

    int width,height,stride;
    vector<float> vimage;
};

class Image_pickle_suite : public pickle_suite
{
public:
    static tuple getinitargs(const PyImage& im)
    {
        return make_tuple(im.width,im.height,im.stride,toPyArrayN(&im.vimage[0],im.vimage.size()));
    }
};

object ReturnKeypoints(Keypoint keypts)
{
    if( keypts == NULL )
        return make_tuple(numeric::array(boost::python::list()).astype("f4"),numeric::array(boost::python::list()).astype("f4"));
    int numkeys = 0;
    Keypoint key = keypts;
    while(key) {
        numkeys++;
        key = key->next;
    }

    npy_intp dims[2] = {numkeys,4};
    PyObject *pyframes = PyArray_SimpleNew(2,dims, PyArray_FLOAT);
    float* pframes = (float*)PyArray_DATA(pyframes);
    
    dims[1] = 128;
    PyObject *pydesc = PyArray_SimpleNew(2,dims, PyArray_FLOAT);
    float* pdesc = (float*)PyArray_DATA(pydesc);

    int index = 0;
    key = keypts;
    while(key) {

        for(int j = 0; j < 128; ++j)
            pdesc[128*index+j] = key->descrip[j];

        pframes[4*index+0] = key->col;
        pframes[4*index+1] = key->row;
        pframes[4*index+2] = key->ori;
        pframes[4*index+3] = key->scale;

        key = key->next;
        ++index;
    }

    return make_tuple(static_cast<numeric::array>(handle<>(pyframes)),static_cast<numeric::array>(handle<>(pydesc)));
}

object PyGetKeypoints(PyImage& im)
{
    struct ImageSt siftimage;
    siftimage.rows = im.height;
    siftimage.cols = im.width;
    siftimage.pixels = &im.vimage[0];
    siftimage.stride = im.stride;
    
    Keypoint keypts = GetKeypoints(&siftimage);
    object o = ReturnKeypoints(keypts);
    FreeKeypoints(keypts);
    DestroyAllImages();
    return o;
}

object PyGetKeypointFrames(PyImage& im)
{
    struct ImageSt siftimage;
    siftimage.rows = im.height;
    siftimage.cols = im.width;
    siftimage.pixels = &im.vimage[0];
    siftimage.stride = im.stride;
    
    Keypoint keypts = GetKeypointFrames(&siftimage);

    int numkeys = 0;
    Keypoint key = keypts;
    while(key) {
        numkeys++;
        key = key->next;
    }

    npy_intp dims[2] = {numkeys,6};
    PyObject *pyframes = PyArray_SimpleNew(2,dims, PyArray_FLOAT);
    float* pframes = (float*)PyArray_DATA(pyframes);
        int index = 0;
    key = keypts;
    while(key) {
        pframes[6*index+0] = key->col;
        pframes[6*index+1] = key->row;
        pframes[6*index+2] = key->ori;
        pframes[6*index+3] = key->scale;
        pframes[6*index+4] = key->imageindex;
        pframes[6*index+5] = key->fpyramidscale;

        key = key->next;
        ++index;
    }

    FreeKeypoints(keypts);
    DestroyAllImages();

    return static_cast<numeric::array>(handle<>(pyframes));
}

object PyGetKeypointDescriptors(PyImage& im,object oframes)
{
    struct ImageSt siftimage;
    siftimage.rows = im.height;
    siftimage.cols = im.width;
    siftimage.pixels = &im.vimage[0];
    siftimage.stride = im.stride;

    vector<Keypoint> vkeypoints(len(oframes));
    for(size_t i = 0; i < vkeypoints.size(); ++i) {
        object oframe = oframes[i];
        Keypoint keypt = (Keypoint)py_aligned_malloc(sizeof(KeypointSt),16);
        keypt->col = extract<float>(oframe[0]);
        keypt->row = extract<float>(oframe[1]);
        keypt->ori = extract<float>(oframe[2]);
        keypt->scale = extract<float>(oframe[3]);
        keypt->imageindex = extract<int>(oframe[4]);
        keypt->fpyramidscale = extract<float>(oframe[5]);
        vkeypoints[i] = keypt;
        if( i > 0 )
            keypt->next = vkeypoints[i-1];
        else
            keypt->next = NULL;
    }

    object o;
    if( vkeypoints.size() > 0 ) {
        GetKeypointDescriptors(&siftimage,vkeypoints.back());
        o = ReturnKeypoints(vkeypoints.back());
        for(size_t i = 0; i < vkeypoints.size(); ++i)
            py_aligned_free(vkeypoints[i]);
    }
    else
        o = ReturnKeypoints(NULL);
    DestroyAllImages();
    return o;
}

object PyGetKeypoints(numeric::array oarray)
{
    PyImage pimage(oarray);
    return PyGetKeypoints(pimage);
}

object PyGetKeypointFrames(numeric::array oarray)
{
    PyImage pimage(oarray);
    return PyGetKeypointFrames(pimage);
}

object PyGetKeypointDescriptors(numeric::array oarray, object oframes)
{
    PyImage pimage(oarray);
    return PyGetKeypointDescriptors(pimage, oframes);
}

struct DummyStruct {};

struct int_from_int
{
    int_from_int()
    {
        converter::registry::push_back(&convertible, &construct, type_id<int>());
    }

    static void* convertible( PyObject* obj)
    {
        PyObject* newobj = PyNumber_Int(obj);
        if (!PyString_Check(obj) && newobj) {
            Py_DECREF(newobj);
            return obj;
        }
        else {
            if (newobj) {
                Py_DECREF(newobj);
            }
            PyErr_Clear();
            return 0;
        }
    }

    static void construct(PyObject* _obj, converter::rvalue_from_python_stage1_data* data)
    {
        PyObject* newobj = PyNumber_Int(_obj);
        int* storage = (int*)((converter::rvalue_from_python_storage<int>*)data)->storage.bytes;
        *storage = extract<int>(newobj);
        Py_DECREF(newobj);
        data->convertible = storage;
    }
};

template<typename T>
struct T_from_number
{
    T_from_number()
    {
        converter::registry::push_back(&convertible, &construct, type_id<T>());
    }

    static void* convertible( PyObject* obj)
    {
        PyObject* newobj = PyNumber_Float(obj);
        if (!PyString_Check(obj) && newobj) {
            Py_DECREF(newobj);
            return obj;
        }
        else {
            if (newobj) {
                Py_DECREF(newobj);
            }
            PyErr_Clear();
            return 0;
        }
    }

    static void construct(PyObject* _obj, converter::rvalue_from_python_stage1_data* data)
    {
        PyObject* newobj = PyNumber_Float(_obj);
        T* storage = (T*)((converter::rvalue_from_python_storage<T>*)data)->storage.bytes;
        *storage = extract<T>(newobj);
        Py_DECREF(newobj);
        data->convertible = storage;
    }
};

BOOST_PYTHON_MODULE(siftfastpy)
{
    import_array();
    numeric::array::set_module_and_type("numpy", "ndarray");
    register_exception_translator<siftfast_exception>(&translate_siftfast_exception);
    int_from_int();
    T_from_number<float>();
    T_from_number<double>();

    def("DestroyAllResources",DestroyAllResources);
    object (*pkeypoints1)(PyImage&) = PyGetKeypoints;
    object (*pkeypoints2)(numeric::array) = PyGetKeypoints;
    def("GetKeypoints",pkeypoints1,args("image"));
    def("GetKeypoints",pkeypoints2,args("array"));
    object (*pkeypointsf1)(PyImage&) = PyGetKeypointFrames;
    object (*pkeypointsf2)(numeric::array) = PyGetKeypointFrames;
    def("GetKeypointFrames",pkeypointsf1,args("image"));
    def("GetKeypointFrames",pkeypointsf2,args("array"));
    object (*pkeypointsd1)(PyImage&,object) = PyGetKeypointDescriptors;
    object (*pkeypointsd2)(numeric::array,object) = PyGetKeypointDescriptors;
    def("GetKeypointDescriptors",pkeypointsd1,args("image","frames"));
    def("GetKeypointDescriptors",pkeypointsd2,args("array","frames"));

    class_<PyImage>("Image", no_init)
        .def(init<int,int>())
        .def(init<object>())
        .def_readonly("width",&PyImage::width)
        .def_readonly("height",&PyImage::height)
        .def("SetData",&PyImage::SetData,args("data"))
        .def_pickle(Image_pickle_suite())
        ;
    def("SetSiftParameters",SetSiftParameters);
    def("GetSiftParameters",GetSiftParameters);

    class_<SiftParameters>("SiftParameters")
        .def_readwrite("DoubleImSize",&SiftParameters::DoubleImSize)
        .def_readwrite("Scales",&SiftParameters::Scales)
        .def_readwrite("InitSigma",&SiftParameters::InitSigma)
        .def_readwrite("PeakThresh",&SiftParameters::PeakThresh)
        ;
}
