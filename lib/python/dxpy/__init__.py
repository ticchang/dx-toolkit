'''
Importing this package will set the security context appropriately for
use with the :func:`dxpy.DXHTTPRequest` function, which will then set
the headers appropriately when communicating with the API server.  In
addition, it will set the default workspace according to
DX_WORKSPACE_ID (if running inside an Execution Environment) or
DX_PROJECT_CONTEXT_ID (otherwise).  This workspace will be used by
default for any object handler methods that require a project ID.

When importing the :mod:`dxpy.bindings` submodule, the methods for
setting the security context etc. do not need to be used directly so
long as the appropriate environment variables DX_APISERVER_HOST,
DX_APISERVER_PORT, and DX_SECURITY_CONTEXT have been properly set.

If an HTTP/HTTPS proxy is to be used, set the environment variables
beforehand as applicable while using the format 'hostname:port'
(e.g. '10.10.1.10:3128'):

* **HTTP_PROXY**: 'hostname:port' for the HTTP proxy
* **HTTPS_PROXY**: 'hostname:port' for the HTTPS proxy

'''

import os, json, requests
from requests.exceptions import ConnectionError, HTTPError
from requests.auth import AuthBase
from dxpy.exceptions import *

snappy_available = True
try:
    import snappy
except ImportError:
    snappy_available = False

#gevent_available = True
#try:
#    import gevent, gevent.pool, gevent.monkey
#    gevent.monkey.patch_all(thread=False, select=False)
#except ImportError:
#    gevent_available = False

API_VERSION = '1.0.0'
AUTH_HELPER = None
JOB_ID, WORKSPACE_ID, PROJECT_CONTEXT_ID = None, None, None

MAX_RETRIES = 0

def DXHTTPRequest(resource, data, method='POST', headers={}, auth=None, config=None,
                  use_compression=None, jsonify_data=True, want_full_response=False, **kwargs):
    '''
    :param resource: API server route, e.g. "/record/new"
    :type resource: string
    :param data: Contents for the request body
    :param jsonify_data: Indicates whether *data* should be converted from a Python list or dict to a JSON string
    :type jsonify_data: boolean
    :param want_full_response: Indicates whether the function should return the full :class:`requests.Response` object or just the content of the response
    :type want_full_response: boolean
    :returns: Response from API server in the requested format.  Note: if *want_full_response* is set to False and the header "content-type" is found in the response with value "application/json", the contents of the response will **always** be converted from JSON to Python before it is returned, and it will therefore be of type list or dict.
    :raises: :exc:`requests.exceptions.HTTPError` if response code was not 200 (OK), :exc:`ValueError` if the response from the API server cannot be decoded

    Wrapper around requests.request(). Inserts authentication and
    converts *data* to JSON.
    '''
    url = APISERVER + resource

    if auth is None:
        auth = AUTH_HELPER
    if config is None:
        config = {}
    # TODO: decide which routes are safe to retry
    # TODO: exponential backoff policy in requests
    # This will make the total number of retries MAX_RETRIES^2 for some errors. TODO: check how to better integrate with requests retry logic.
    # config.setdefault('max_retries', MAX_RETRIES)
    if 'Content-Type' not in headers:
        headers['Content-Type'] = 'application/json'
    if jsonify_data:
        data = json.dumps(data)

    headers['DNAnexus-API'] = API_VERSION

    if use_compression == 'snappy':
        if not snappy_available:
            raise DXError("Snappy compression requested, but the snappy module is unavailable")
        headers['accept-encoding'] = 'snappy'

    last_error = None
    for retry in range(MAX_RETRIES + 1):
        try:
            response = requests.request(method, url, data=data, headers=headers,
                                        auth=auth, config=config, **kwargs)

            # If HTTP code that is not 200 (OK) is received and the content is
            # JSON, parse it and throw the appropriate error.  Otherwise,
            # raise the usual exception.
            if response.status_code != requests.codes.ok:
                # response.headers key lookup is case-insensitive
                if response.headers.get('content-type', '').startswith('application/json'):
                    content = json.loads(response.content)
                    raise DXAPIError(content["error"]["type"],
                                     content["error"]["message"],
                                     response.status_code)
                response.raise_for_status()

            if want_full_response:
                return response
            else:
                if 'content-length' in response.headers:
                    if int(response.headers['content-length']) != len(response.content):
                        raise HTTPError("Received response with content-length header set to %s but content length is %d"
                            % (response.headers['content-length'], len(response.content)))

                if use_compression and response.headers.get('content-encoding', '') == 'snappy':
                    decoded_content = snappy.uncompress(response.content)
                else:
                    decoded_content = response.content

                if response.headers.get('content-type', '').startswith('application/json'):
                    return json.loads(decoded_content)
                return decoded_content
        except ConnectionError as e:
            last_error = e
        except (DXAPIError, HTTPError) as e:
            last_error = e
            if method != 'GET' and response.status_code != requests.codes.server_error:
                break # Disable retries
        if last_error is None:
            last_error = DXError("Internal error in DXHTTPRequest")
    raise last_error

class DXHTTPOAuth2(AuthBase):
    def __init__(self, security_context):
        self.security_context = security_context

    def __call__(self, r):
        if self.security_context["auth_token_type"].lower() == 'bearer':
            r.headers['Authorization'] = \
                self.security_context["auth_token_type"] + " " + \
                self.security_context["auth_token"]
        else:
            raise NotImplementedError("Token types other than bearer are not yet supported")
        return r

def set_api_server_info(host='localhost', port=8124, protocol='http'):
    global APISERVER_HOST, APISERVER_PORT, APISERVER
    APISERVER_HOST = host
    APISERVER_PORT = port
    APISERVER = protocol + "://" + host + ":" + str(port)

def set_security_context(security_context):
    global SECURITY_CONTEXT, AUTH_HELPER
    SECURITY_CONTEXT = security_context
    AUTH_HELPER = DXHTTPOAuth2(security_context)

def set_job_id(dxid):
    """
    :param id: ID of a job
    :type id: string

    Sets the ID of the running job.  TODO: Not clear yet that this is
    necessary.

    """
    global JOB_ID
    JOB_ID = dxid

def set_workspace_id(dxid):
    """
    :param id: ID of a project or workspace
    :type id: string

    Sets the default project or workspace for object creation and
    modification to *id*.
    """

    global WORKSPACE_ID
    WORKSPACE_ID = dxid

def set_project_context(dxid):
    """
    :param id: Project ID
    :type id: string

    Sets the project context for a running job.

    """

    global PROJECT_CONTEXT_ID
    PROJECT_CONTEXT_ID = dxid

if "DX_APISERVER_HOST" in os.environ and "DX_APISERVER_PORT" in os.environ:
    set_api_server_info(host=os.environ["DX_APISERVER_HOST"], port=os.environ["DX_APISERVER_PORT"])
else:
    set_api_server_info()

if "DX_SECURITY_CONTEXT" in os.environ:
    set_security_context(json.loads(os.environ['DX_SECURITY_CONTEXT']))
else:
    print "Warning: no security context found in environment variables"

if "DX_JOB_ID" in os.environ:
    set_job_id(os.environ["DX_JOB_ID"])
    if "DX_WORKSPACE_ID" in os.environ:
        set_workspace_id(os.environ["DX_WORKSPACE_ID"])
    if "DX_PROJECT_CONTEXT_ID" in os.environ:
        set_project_context(os.environ["DX_PROJECT_CONTEXT_ID"])
else:
    if "DX_PROJECT_CONTEXT_ID" in os.environ:
        set_workspace_id(os.environ["DX_PROJECT_CONTEXT_ID"])

from dxpy.bindings import *
from dxpy.dxlog import *
