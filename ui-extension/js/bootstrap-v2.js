/*
 * guacamole-espkvm
 *
 * Browser-side bootstrap for the ESPKVM Guacamole integration.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

(function () {
    "use strict";

    var api = window.__GUACAMOLE_ESPKVM__ = {
        loaded: true,
        version: "0.1.0",
        clients: [],
        activeClient: null
    };

    console.info("[guacamole-espkvm] Browser extension loaded.");

    if (!window.angular) {
        console.error("[guacamole-espkvm] AngularJS is not available.");
        return;
    }

    angular.module("client").config(["$provide", function ($provide) {

        $provide.decorator("ManagedClient", [
            "$delegate",
            "$injector",
            function ($delegate, $injector) {

                var originalGetInstance = $delegate.getInstance;

                $delegate.getInstance = function (id) {

                    var managedClient = originalGetInstance.call($delegate, id);
                    var tunnel = managedClient.tunnel;
                    var originalOnUuid = tunnel.onuuid;

                    tunnel.onuuid = function (uuid) {

                        if (originalOnUuid)
                            originalOnUuid.call(tunnel, uuid);

                        var tunnelService = $injector.get("tunnelService");

                        tunnelService.getProtocol(uuid).then(function (protocol) {

                            if (!protocol || protocol.name !== "espkvm")
                                return;

                            managedClient.__espkvm = true;
                            managedClient.__espkvmUuid = uuid;

                            api.activeClient = managedClient;

                            api.clients.push({
                                id: managedClient.id,
                                uuid: uuid,
                                protocol: protocol.name
                            });

                            console.info(
                                "[guacamole-espkvm] ESPKVM connection detected:",
                                managedClient.id,
                                uuid
                            );

                            var client = managedClient.client;
                            var previousOnPipe = client.onpipe;

                            client.onpipe = function (stream, mimetype, name) {

                                if (name === "espkvm:echo:reply") {

                                    console.info(
                                        "[guacamole-espkvm] Echo reply pipe received."
                                    );

                                    var reader = new Guacamole.StringReader(stream);

                                    reader.ontext = function (text) {
                                        if (api._echoResolve) {
                                            api._echoBuffer += text;
                                        }
                                    };

                                    reader.onend = function () {
                                        if (api._echoResolve) {
                                            var resolve = api._echoResolve;
                                            var result = api._echoBuffer;

                                            api._echoResolve = null;
                                            api._echoReject = null;
                                            api._echoBuffer = "";

                                            resolve(result);
                                        }
                                    };

                                    return;
                                }




                                /*
                                 * Incoming binary data from ESPKVM /ws.
                                 */
                                /*
                                 * Incoming framed ESPKVM native MJPEG data.
                                 */
                                if (name
                                        && /^espkvm:mjpeg-stream:[0-9]+:rx$/.test(name)) {

                                    api._acceptMjpegRx(
                                        stream,
                                        mimetype,
                                        name
                                    );

                                    return;
                                }

                                /*
                                 * Incoming framed ESPKVM /video data.
                                 */
                                if (name
                                        && /^espkvm:video-ws:[0-9]+:rx$/.test(name)) {

                                    api._acceptVideoWsRx(
                                        stream,
                                        mimetype,
                                        name
                                    );

                                    return;
                                }

                                if (name
                                        && /^espkvm:control-ws:[0-9]+:rx$/.test(name)) {

                                    api._acceptControlWsRx(
                                        stream,
                                        mimetype,
                                        name
                                    );

                                    return;
                                }

                                if (name && name.indexOf("espkvm:http:") === 0
                                        && name.endsWith(":reply")) {

                                    var match = name.match(
                                        /^espkvm:http:([0-9]+):reply$/
                                    );

                                    if (!match)
                                        return;

                                    var requestId = match[1];
                                    var pending = api._httpPending[requestId];

                                    if (!pending)
                                        return;

                                    var reader = new Guacamole.StringReader(stream);
                                    var buffer = "";

                                    reader.ontext = function (text) {
                                        buffer += text;
                                    };

                                    reader.onend = function () {

                                        delete api._httpPending[requestId];

                                        try {
                                            pending.resolve(JSON.parse(buffer));
                                        }
                                        catch (error) {
                                            pending.reject(error);
                                        }
                                    };

                                    return;
                                }

                                if (name === "espkvm:session-test:reply") {

                                    console.info(
                                        "[guacamole-espkvm] Session-test reply pipe received."
                                    );

                                    var sessionReader = new Guacamole.StringReader(stream);

                                    sessionReader.ontext = function (text) {
                                        if (api._sessionResolve)
                                            api._sessionBuffer += text;
                                    };

                                    sessionReader.onend = function () {

                                        if (!api._sessionResolve)
                                            return;

                                        var resolve = api._sessionResolve;
                                        var reject = api._sessionReject;
                                        var raw = api._sessionBuffer;

                                        api._sessionResolve = null;
                                        api._sessionReject = null;
                                        api._sessionBuffer = "";

                                        try {
                                            resolve(JSON.parse(raw));
                                        }
                                        catch (error) {
                                            reject(error);
                                        }
                                    };

                                    return;
                                }

                                if (name && name.indexOf("espkvm:") === 0) {
                                    console.info(
                                        "[guacamole-espkvm] Incoming ESPKVM pipe:",
                                        name,
                                        mimetype
                                    );
                                    return;
                                }

                                if (previousOnPipe)
                                    return previousOnPipe.call(
                                        client,
                                        stream,
                                        mimetype,
                                        name
                                    );
                            };

                        }, function (error) {
                            console.warn(
                                "[guacamole-espkvm] Could not determine tunnel protocol.",
                                error
                            );
                        });
                    };

                    return managedClient;
                };

                return $delegate;
            }
        ]);
    }]);

    api.echo = function (text) {

        return new Promise(function (resolve, reject) {

            if (!api.activeClient) {
                reject(new Error("No active ESPKVM connection."));
                return;
            }

            if (api._echoResolve) {
                reject(new Error("Another ESPKVM echo test is already running."));
                return;
            }

            api._echoResolve = resolve;
            api._echoReject = reject;
            api._echoBuffer = "";

            var stream = api.activeClient.client.createPipeStream(
                "text/plain",
                "espkvm:echo"
            );

            var writer = new Guacamole.StringWriter(stream);

            writer.sendText(String(text));
            writer.sendEnd();

            setTimeout(function () {
                if (api._echoReject === reject) {

                    api._echoResolve = null;
                    api._echoReject = null;
                    api._echoBuffer = "";

                    reject(new Error("ESPKVM echo timed out."));
                }
            }, 5000);
        });
    };


    api.sessionTest = function () {

        return new Promise(function (resolve, reject) {

            if (!api.activeClient) {
                reject(new Error("No active ESPKVM connection."));
                return;
            }

            if (api._sessionResolve) {
                reject(new Error("Another ESPKVM session test is already running."));
                return;
            }

            api._sessionResolve = resolve;
            api._sessionReject = reject;
            api._sessionBuffer = "";

            var stream = api.activeClient.client.createPipeStream(
                "application/json",
                "espkvm:session-test"
            );

            var writer = new Guacamole.StringWriter(stream);
            writer.sendEnd();

            setTimeout(function () {

                if (api._sessionReject === reject) {

                    api._sessionResolve = null;
                    api._sessionReject = null;
                    api._sessionBuffer = "";

                    reject(new Error("ESPKVM session test timed out."));
                }

            }, 5000);

        });
    };



    api._httpCounter = 0;
    api._httpPending = {};

    api.httpGet = function (path) {

        return new Promise(function (resolve, reject) {

            if (!api.activeClient) {
                reject(new Error("No active ESPKVM connection."));
                return;
            }

            if (typeof path !== "string"
                    || (path !== "/" && !path.startsWith("/api/"))) {
                reject(new Error("Only / or /api/... paths are allowed."));
                return;
            }

            api._httpCounter++;

            var requestId = String(api._httpCounter);

            api._httpPending[requestId] = {
                resolve: resolve,
                reject: reject
            };

            var stream = api.activeClient.client.createPipeStream(
                "text/plain",
                "espkvm:http:" + requestId
            );

            var writer = new Guacamole.StringWriter(stream);

            writer.sendText(path);
            writer.sendEnd();

            setTimeout(function () {

                var pending = api._httpPending[requestId];

                if (!pending)
                    return;

                delete api._httpPending[requestId];

                pending.reject(
                    new Error(
                        "ESPKVM HTTP request " + requestId + " timed out."
                    )
                );

            }, 10000);

        });
    };




    /*
     * HTTP GET bound to a specific ManagedClient.
     *
     * This is required because multiple ESPKVM connections may exist at the
     * same time. Requests must never depend only on the globally focused client.
     */
    api._httpGetForClient = function (managedClient, path) {

        return new Promise(function (resolve, reject) {

            if (!managedClient || !managedClient.client) {
                reject(new Error("Invalid ESPKVM client."));
                return;
            }

            if (typeof path !== "string"
                    || (path !== "/" && !path.startsWith("/api/"))) {
                reject(new Error("Only / or /api/... paths are allowed."));
                return;
            }

            api._httpCounter++;

            var requestId = String(api._httpCounter);

            api._httpPending[requestId] = {
                resolve: resolve,
                reject: reject
            };

            var stream = managedClient.client.createPipeStream(
                "text/plain",
                "espkvm:http:" + requestId
            );

            var writer = new Guacamole.StringWriter(stream);

            writer.sendText(path);
            writer.sendEnd();

            setTimeout(function () {

                var pending = api._httpPending[requestId];

                if (!pending)
                    return;

                delete api._httpPending[requestId];

                pending.reject(
                    new Error(
                        "ESPKVM HTTP request "
                        + requestId
                        + " timed out."
                    )
                );

            }, 15000);

        });
    };


    /*
     * Produces the ESPKVM document used inside the connection iframe.
     *
     * The adapter is inserted BEFORE ESPKVM's main Vue bundle so that the
     * console sees our fetch implementation from its first API request.
     */
    api._prepareEspkvmHtml = function (html, token) {

        var adapter = `
<script>
(function () {
    "use strict";

    var TOKEN = ${JSON.stringify(token)};
    var bridge = window.parent.__GUACAMOLE_ESPKVM__;
    var realFetch = window.fetch.bind(window);
    var RealWebSocket = window.WebSocket;

    /*
     * WebSocket implementation backed by the active Guacamole connection.
     *
     * For now only /ws is proxied. /video will use the same transport in
     * the next stage.
     */
    class GuacamoleControlWebSocket extends EventTarget {

        constructor(url) {
            super();

            this.url = String(url);
            this.readyState = RealWebSocket.CONNECTING;
            this.binaryType = "blob";
            this.bufferedAmount = 0;
            this.extensions = "";
            this.protocol = "";

            this.onopen = null;
            this.onmessage = null;
            this.onerror = null;
            this.onclose = null;

            this._id = null;
            this._closeRequested = false;

            var self = this;

            bridge._frameControlWsOpen(
                TOKEN,
                {
                    ondata: function (buffer) {

                        if (self.readyState
                                !== RealWebSocket.OPEN) {
                            return;
                        }

                        var sourceBytes =
                            new Uint8Array(buffer);

                        var localBytes =
                            new Uint8Array(
                                sourceBytes.byteLength
                            );

                        localBytes.set(sourceBytes);

                        var localBuffer =
                            localBytes.buffer;

                        var data =
                            self.binaryType === "arraybuffer"
                                ? localBuffer
                                : new Blob([localBuffer]);

                        var event =
                            new MessageEvent(
                                "message",
                                { data: data }
                            );

                        self.dispatchEvent(event);

                        if (typeof self.onmessage
                                === "function") {
                            self.onmessage(event);
                        }
                    },

                    onclose: function () {
                        self._remoteClose();
                    }
                }
            )
            .then(function (id) {

                self._id = id;

                if (self._closeRequested) {
                    bridge._frameControlWsClose(
                        TOKEN,
                        id
                    );

                    self._remoteClose();
                    return;
                }

                self.readyState =
                    RealWebSocket.OPEN;

                var event =
                    new Event("open");

                self.dispatchEvent(event);

                if (typeof self.onopen
                        === "function") {
                    self.onopen(event);
                }
            })
            .catch(function (error) {

                if (self.readyState
                        === RealWebSocket.CLOSED) {
                    return;
                }

                var errorEvent =
                    new Event("error");

                self.dispatchEvent(
                    errorEvent
                );

                if (typeof self.onerror
                        === "function") {
                    self.onerror(
                        errorEvent
                    );
                }

                self._remoteClose();

                console.error(
                    "[guacamole-espkvm] /ws failed:",
                    error
                );
            });
        }


        send(data) {

            if (this.readyState
                    !== RealWebSocket.OPEN) {

                throw new DOMException(
                    "WebSocket is not open.",
                    "InvalidStateError"
                );
            }

            var buffer;

            if (data instanceof ArrayBuffer) {
                buffer = data;
            }

            else if (ArrayBuffer.isView(data)) {

                /*
                 * Copy exactly the visible portion of the typed array.
                 */
                buffer = data.buffer.slice(
                    data.byteOffset,
                    data.byteOffset
                        + data.byteLength
                );
            }

            else {
                throw new TypeError(
                    "ESPKVM /ws only supports binary messages."
                );
            }

            bridge._frameControlWsSend(
                TOKEN,
                this._id,
                buffer
            );
        }


        close() {

            if (this.readyState
                    === RealWebSocket.CLOSED
                    || this.readyState
                    === RealWebSocket.CLOSING) {
                return;
            }

            this._closeRequested = true;

            if (this.readyState
                    === RealWebSocket.CONNECTING) {
                return;
            }

            this.readyState =
                RealWebSocket.CLOSING;

            bridge._frameControlWsClose(
                TOKEN,
                this._id
            );
        }


        _remoteClose() {

            if (this.readyState
                    === RealWebSocket.CLOSED) {
                return;
            }

            this.readyState =
                RealWebSocket.CLOSED;

            var event;

            try {
                event = new CloseEvent(
                    "close",
                    {
                        code: 1000,
                        reason: "",
                        wasClean: true
                    }
                );
            }
            catch (e) {
                event = new Event("close");
            }

            this.dispatchEvent(event);

            if (typeof this.onclose
                    === "function") {
                this.onclose(event);
            }
        }
    }



    /*
     * WebSocket-compatible object for ESPKVM /video.
     *
     * The parent Guacamole page reassembles each complete ESPKVM video
     * WebSocket message before delivering it here.
     */
    class GuacamoleVideoWebSocket extends EventTarget {

        constructor(url) {
            super();

            this.url = String(url);
            this.readyState = RealWebSocket.CONNECTING;
            this.binaryType = "blob";
            this.bufferedAmount = 0;
            this.extensions = "";
            this.protocol = "";

            this.onopen = null;
            this.onmessage = null;
            this.onerror = null;
            this.onclose = null;

            this._id = null;
            this._closeRequested = false;

            var self = this;

            bridge._frameVideoWsOpen(
                TOKEN,
                {
                    ondata: function (buffer) {

                        if (self.readyState
                                !== RealWebSocket.OPEN) {
                            return;
                        }

                        var sourceBytes =
                            new Uint8Array(buffer);

                        var localBytes =
                            new Uint8Array(sourceBytes.byteLength);

                        localBytes.set(sourceBytes);

                        var localBuffer =
                            localBytes.buffer;

                        var data =
                            self.binaryType === "arraybuffer"
                                ? localBuffer
                                : new Blob([localBuffer]);

                        var event =
                            new MessageEvent(
                                "message",
                                { data: data }
                            );

                        self.dispatchEvent(event);

                        if (typeof self.onmessage
                                === "function") {
                            self.onmessage(event);
                        }
                    },

                    onclose: function () {
                        self._remoteClose();
                    }
                }
            )
            .then(function (id) {

                self._id = id;

                if (self._closeRequested) {

                    bridge._frameVideoWsClose(
                        TOKEN,
                        id
                    );

                    self._remoteClose();
                    return;
                }

                self.readyState =
                    RealWebSocket.OPEN;

                var event =
                    new Event("open");

                self.dispatchEvent(event);

                if (typeof self.onopen
                        === "function") {
                    self.onopen(event);
                }
            })
            .catch(function (error) {

                if (self.readyState
                        === RealWebSocket.CLOSED) {
                    return;
                }

                var errorEvent =
                    new Event("error");

                self.dispatchEvent(errorEvent);

                if (typeof self.onerror
                        === "function") {
                    self.onerror(errorEvent);
                }

                self._remoteClose();

                console.error(
                    "[guacamole-espkvm] /video failed:",
                    error
                );
            });
        }


        send(data) {

            if (this.readyState
                    !== RealWebSocket.OPEN) {

                throw new DOMException(
                    "WebSocket is not open.",
                    "InvalidStateError"
                );
            }

            var buffer;

            if (data instanceof ArrayBuffer) {
                buffer = data;
            }

            else if (ArrayBuffer.isView(data)) {

                buffer = data.buffer.slice(
                    data.byteOffset,
                    data.byteOffset
                        + data.byteLength
                );
            }

            else {
                throw new TypeError(
                    "ESPKVM /video only supports binary messages."
                );
            }

            bridge._frameVideoWsSend(
                TOKEN,
                this._id,
                buffer
            );
        }


        close() {

            if (this.readyState
                    === RealWebSocket.CLOSED
                    || this.readyState
                    === RealWebSocket.CLOSING) {
                return;
            }

            this._closeRequested = true;

            if (this.readyState
                    === RealWebSocket.CONNECTING) {
                return;
            }

            this.readyState =
                RealWebSocket.CLOSING;

            bridge._frameVideoWsClose(
                TOKEN,
                this._id
            );
        }


        _remoteClose() {

            if (this.readyState
                    === RealWebSocket.CLOSED) {
                return;
            }

            this.readyState =
                RealWebSocket.CLOSED;

            var event;

            try {
                event = new CloseEvent(
                    "close",
                    {
                        code: 1000,
                        reason: "",
                        wasClean: true
                    }
                );
            }
            catch (e) {
                event = new Event("close");
            }

            this.dispatchEvent(event);

            if (typeof this.onclose
                    === "function") {
                this.onclose(event);
            }
        }
    }


    /*
     * Preserve native WebSocket behavior for everything except ESPKVM /ws.
     */
    window.WebSocket = new Proxy(
        RealWebSocket,
        {
            construct: function (
                    target,
                    args) {

                var url =
                    String(args[0]);

                /*
                 * In srcdoc ESPKVM currently constructs "ws:///ws".
                 * Match by pathname suffix instead of requiring a valid URL.
                 */
                var cleanUrl =
                    url.split("#")[0].split("?")[0];

                if (cleanUrl.slice(-3) === "/ws") {

                    return new GuacamoleControlWebSocket(
                        url
                    );
                }

                if (cleanUrl.slice(-6) === "/video") {

                    return new GuacamoleVideoWebSocket(
                        url
                    );
                }

                return new target(...args);
            }
        }
    );



    /*
     * Native ESPKVM MJPEG fallback.
     *
     * The native console uses an img element whose source is /stream.
     * Inside Guacamole that URL must not be requested from the Guacamole
     * origin. Instead, open the dedicated MJPEG pipe and feed each JPEG
     * frame to the original img element through a local Blob URL.
     */
    var nativeImageSrc =
        Object.getOwnPropertyDescriptor(
            HTMLImageElement.prototype,
            "src"
        );

    var nativeSetAttribute =
        Element.prototype.setAttribute;

    var nativeRemoveAttribute =
        Element.prototype.removeAttribute;

    var mjpegImageStates =
        new WeakMap();


    function isMjpegStreamSource(value) {

        var url =
            String(value || "");

        var clean =
            url.split("#")[0].split("?")[0];

        return clean === "/stream"
            || clean.slice(-7) === "/stream";
    }


    function stopMjpegImage(img) {

        var state =
            mjpegImageStates.get(img);

        if (!state)
            return;

        state.active = false;
        state.pending = null;

        if (state.id !== null) {

            try {
                bridge._frameMjpegClose(
                    TOKEN,
                    state.id
                );
            }
            catch (e) {}

            state.id = null;
        }

        if (state.currentUrl) {

            try {
                URL.revokeObjectURL(
                    state.currentUrl
                );
            }
            catch (e) {}

            state.currentUrl = null;
        }

        mjpegImageStates.delete(img);
    }


    function renderMjpegFrame(
            img,
            state,
            buffer) {

        if (!state.active)
            return;

        /*
         * Do not queue decoded images faster than the browser can display
         * them. Keep only the newest frame while one image is loading.
         */
        if (state.busy) {
            state.pending = buffer;
            return;
        }

        state.busy = true;

        /*
         * Copy into the iframe realm, just like the WebSocket bridges.
         */
        var sourceBytes =
            new Uint8Array(buffer);

        var localBytes =
            new Uint8Array(
                sourceBytes.byteLength
            );

        localBytes.set(sourceBytes);

        var blob =
            new Blob(
                [localBytes.buffer],
                { type: "image/jpeg" }
            );

        var objectUrl =
            URL.createObjectURL(blob);

        var previousUrl =
            state.currentUrl;

        state.currentUrl =
            objectUrl;

        var settled = false;

        function finishFrame() {

            if (settled)
                return;

            settled = true;

            img.removeEventListener(
                "load",
                finishFrame
            );

            img.removeEventListener(
                "error",
                finishFrame
            );

            if (previousUrl) {

                try {
                    URL.revokeObjectURL(
                        previousUrl
                    );
                }
                catch (e) {}
            }

            state.busy = false;

            if (!state.active)
                return;

            var pending =
                state.pending;

            state.pending = null;

            if (pending) {

                renderMjpegFrame(
                    img,
                    state,
                    pending
                );
            }
        }

        img.addEventListener(
            "load",
            finishFrame
        );

        img.addEventListener(
            "error",
            finishFrame
        );

        nativeImageSrc.set.call(
            img,
            objectUrl
        );

        /*
         * Backstop for a browser that neither loads nor rejects a replaced
         * frame. This also prevents the newest-frame queue from stalling.
         */
        window.setTimeout(
            finishFrame,
            2000
        );
    }


    function startMjpegImage(
            img,
            source) {

        stopMjpegImage(img);

        var state = {
            active: true,
            id: null,
            source: String(source),
            busy: false,
            pending: null,
            currentUrl: null
        };

        mjpegImageStates.set(
            img,
            state
        );

        bridge._frameMjpegOpen(
            TOKEN,
            {
                onframe: function (buffer) {

                    if (!state.active)
                        return;

                    renderMjpegFrame(
                        img,
                        state,
                        buffer
                    );
                },

                onclose: function () {

                    state.id = null;

                    if (!state.active)
                        return;

                    /*
                     * A codec switch ends /stream cleanly. Native ESPKVM
                     * keeps the final frame until its codec watcher moves
                     * back to /video, so do the same here.
                     *
                     * If the image is still visible after a short delay,
                     * treat the end as a real MJPEG failure so the native
                     * retry logic can run.
                     */
                    window.setTimeout(
                        function () {

                            if (!state.active
                                    || mjpegImageStates.get(img)
                                        !== state) {
                                return;
                            }

                            if (img.style.display === "none")
                                return;

                            img.dispatchEvent(
                                new Event("error")
                            );
                        },
                        1500
                    );
                }
            }
        )
        .then(function (id) {

            if (!state.active) {

                bridge._frameMjpegClose(
                    TOKEN,
                    id
                );

                return;
            }

            state.id = id;
        })
        .catch(function (error) {

            if (!state.active)
                return;

            console.error(
                "[guacamole-espkvm] /stream failed:",
                error
            );

            img.dispatchEvent(
                new Event("error")
            );
        });
    }


    /*
     * Vue normally assigns img.src as a DOM property.
     */
    if (nativeImageSrc
            && nativeImageSrc.get
            && nativeImageSrc.set) {

        Object.defineProperty(
            HTMLImageElement.prototype,
            "src",
            {
                configurable:
                    nativeImageSrc.configurable,

                enumerable:
                    nativeImageSrc.enumerable,

                get:
                    nativeImageSrc.get,

                set: function (value) {

                    if (isMjpegStreamSource(
                            value)) {

                        startMjpegImage(
                            this,
                            value
                        );

                        return;
                    }

                    stopMjpegImage(this);

                    nativeImageSrc.set.call(
                        this,
                        value
                    );
                }
            }
        );
    }


    /*
     * Keep a fallback for code which assigns the src attribute directly.
     */
    Element.prototype.setAttribute =
        function (name, value) {

            if (this instanceof HTMLImageElement
                    && String(name).toLowerCase()
                        === "src") {

                if (isMjpegStreamSource(
                        value)) {

                    startMjpegImage(
                        this,
                        value
                    );

                    return;
                }

                stopMjpegImage(this);
            }

            return nativeSetAttribute.call(
                this,
                name,
                value
            );
        };


    Element.prototype.removeAttribute =
        function (name) {

            if (this instanceof HTMLImageElement
                    && String(name).toLowerCase()
                        === "src") {

                stopMjpegImage(this);
            }

            return nativeRemoveAttribute.call(
                this,
                name
            );
        };



    /*
     * Prevent ESPKVM's service worker from being registered against the
     * Guacamole origin.
     */
    if (navigator.serviceWorker) {
        try {
            navigator.serviceWorker.register = function () {
                return Promise.reject(
                    new Error("Service worker disabled inside Guacamole.")
                );
            };
        }
        catch (e) {}
    }

    window.fetch = async function (input, init) {

        var syntheticBase =
            "https://guacamole-espkvm.invalid/";

        var requestUrl;
        var method = "GET";
        var body = null;
        var requestHeaders = new Headers();

        /*
         * Support both fetch("/api/...", {...}) and fetch(Request).
         */
        if (input instanceof Request) {

            requestUrl = new URL(
                input.url,
                syntheticBase
            );

            method = input.method || "GET";

            input.headers.forEach(
                function (value, key) {
                    requestHeaders.set(key, value);
                }
            );
        }
        else {

            requestUrl = new URL(
                String(input),
                syntheticBase
            );
        }

        /*
         * Options passed directly to fetch() override Request values.
         */
        if (init) {

            if (init.method)
                method = init.method;

            if (init.headers) {

                var initHeaders =
                    new Headers(init.headers);

                initHeaders.forEach(
                    function (value, key) {
                        requestHeaders.set(key, value);
                    }
                );
            }

            if (init.body !== undefined
                    && init.body !== null) {

                if (typeof init.body === "string")
                    body = init.body;

                else if (init.body instanceof URLSearchParams)
                    body = init.body.toString();

                else if (init.body instanceof Blob)
                    body = await init.body.text();

                else
                    body = String(init.body);
            }
        }

        method = String(method).toUpperCase();

        /*
         * If a Request object contains the body and init.body did not
         * override it, read a clone so the original remains untouched.
         */
        if (body === null
                && input instanceof Request
                && method !== "GET"
                && method !== "HEAD") {

            try {
                body = await input.clone().text();
            }
            catch (error) {
                body = "";
            }
        }

        /*
         * Route ESPKVM API calls through the authenticated Guacamole
         * connection. The browser never receives the ESPKVM session cookie.
         */
        if (requestUrl.pathname.startsWith("/api/")) {

            var contentType =
                requestHeaders.get("Content-Type") || "";

            var result =
                await bridge._frameHttpRequest(
                    TOKEN,
                    method,
                    requestUrl.pathname
                        + requestUrl.search,
                    body === null ? "" : body,
                    contentType
                );

            var responseHeaders =
                new Headers();

            if (result.contentType)
                responseHeaders.set(
                    "Content-Type",
                    result.contentType
                );

            return new Response(
                result.body || "",
                {
                    status:
                        result.status > 0
                            ? result.status
                            : 502,

                    headers:
                        responseHeaders
                }
            );
        }

        /*
         * Non-ESPKVM requests, such as GitHub release checks, continue to
         * use the browser's normal fetch implementation.
         */
        return realFetch(input, init);
    };

})();
</script>
`;

        var firstScript = html.search(/<script\b/i);

        if (firstScript >= 0)
            return html.slice(0, firstScript)
                + adapter
                + html.slice(firstScript);

        return html.replace("</head>", adapter + "</head>");
    };


    api._frameClients = {};
    api._frameCounter = 0;

    api._frameHttpGet = function (token, path) {

        var managedClient = api._frameClients[token];

        if (!managedClient)
            return Promise.reject(
                new Error("Unknown ESPKVM frame token.")
            );

        return api._httpGetForClient(
            managedClient,
            path
        );
    };


    api._frameHttpRequest = function (
            token,
            method,
            path,
            body,
            contentType) {

        var managedClient = api._frameClients[token];

        if (!managedClient)
            return Promise.reject(
                new Error("Unknown ESPKVM frame token.")
            );

        return api._httpRequestForClient(
            managedClient,
            method,
            path,
            body,
            contentType
        );
    };


    api._mountNativeConsole = async function (
            managedClient,
            rootElement) {

        if (!managedClient
                || !managedClient.__espkvm
                || managedClient.__espkvmMounted)
            return;

        managedClient.__espkvmMounted = true;

        var display =
            rootElement.querySelector(".display");

        var displayOuter =
            rootElement.querySelector(".displayOuter");

        var displayMiddle =
            rootElement.querySelector(".displayMiddle");

        if (!display) {
            managedClient.__espkvmMounted = false;
            throw new Error(
                "Guacamole display container was not found."
            );
        }

        try {

            var result = await api._httpGetForClient(
                managedClient,
                "/"
            );

            if (result.status !== 200)
                throw new Error(
                    "ESPKVM console returned HTTP "
                    + result.status
                );

            api._frameCounter++;

            var token =
                "frame-" + String(api._frameCounter);

            api._frameClients[token] = managedClient;

            var iframe =
                document.createElement("iframe");

            iframe.className =
                "guacamole-espkvm-native-console";

            iframe.title =
                "ESPKVM Console";

            iframe.style.width = "100%";
            iframe.style.height = "100%";
            iframe.style.border = "0";
            iframe.style.display = "block";
            iframe.style.background = "#0e1116";

            /*
             * Ensure Guacamole's display centering/scaling does not constrain
             * the native ESPKVM web console.
             */
            if (displayOuter) {
                displayOuter.style.width = "100%";
                displayOuter.style.height = "100%";
            }

            if (displayMiddle) {
                displayMiddle.style.width = "100%";
                displayMiddle.style.height = "100%";
            }

            display.style.width = "100%";
            display.style.height = "100%";
            display.style.margin = "0";
            display.style.transform = "none";

            display.innerHTML = "";

            display.appendChild(iframe);

            iframe.srcdoc =
                api._prepareEspkvmHtml(
                    result.body,
                    token
                );

            console.info(
                "[guacamole-espkvm] Native ESPKVM console mounted."
            );

        }
        catch (error) {

            managedClient.__espkvmMounted = false;

            console.error(
                "[guacamole-espkvm] Unable to mount native console.",
                error
            );
        }
    };


    /*
     * Extend Guacamole's existing <guac-client> directive without modifying
     * Guacamole core files.
     */
    angular.module("client").config([
        "$provide",
        function ($provide) {

            $provide.decorator(
                "guacClientDirective",
                [
                    "$delegate",
                    function ($delegate) {

                        var directive = $delegate[0];

                        var originalLink =
                            directive.link;

                        directive.link = function (
                                scope,
                                element,
                                attrs) {

                            if (originalLink)
                                originalLink.apply(
                                    this,
                                    arguments
                                );

                            scope.$watch(
                                function () {
                                    return scope.client
                                        && scope.client.__espkvm;
                                },
                                function (isEspkvm) {

                                    if (!isEspkvm)
                                        return;

                                    scope.$evalAsync(
                                        function () {
                                            api._mountNativeConsole(
                                                scope.client,
                                                element[0]
                                            );
                                        }
                                    );
                                }
                            );

                            scope.$on(
                                "$destroy",
                                function () {

                                    var client =
                                        scope.client;

                                    if (!client)
                                        return;

                                    Object.keys(
                                        api._frameClients
                                    ).forEach(
                                        function (token) {

                                            if (api._frameClients[token]
                                                    === client) {
                                                delete api._frameClients[token];
                                            }
                                        }
                                    );
                                }
                            );
                        };

                        return $delegate;
                    }
                ]
            );
        }
    ]);




    /*
     * Reliable post-link hook for <guac-client>.
     *
     * AngularJS normalizes directives before $provide decorators receive
     * them. Changing directive.link at that point is too late because the
     * existing compile function has already been generated. Wrap compile
     * instead and append our own post-link function.
     */
    angular.module("client").config([
        "$provide",
        function ($provide) {

            $provide.decorator(
                "guacClientDirective",
                [
                    "$delegate",
                    function ($delegate) {

                        var directive = $delegate[0];

                        if (directive.__espkvmCompileHookInstalled)
                            return $delegate;

                        directive.__espkvmCompileHookInstalled = true;

                        var originalCompile = directive.compile;

                        directive.compile = function () {

                            var originalLink = originalCompile
                                ? originalCompile.apply(this, arguments)
                                : null;

                            var espkvmPostLink = function (
                                    scope,
                                    element) {

                                var unwatch = scope.$watch(
                                    function () {
                                        return scope.client
                                            && scope.client.__espkvm;
                                    },
                                    function (isEspkvm) {

                                        if (!isEspkvm)
                                            return;

                                        scope.$evalAsync(
                                            function () {

                                                api._mountNativeConsole(
                                                    scope.client,
                                                    element[0]
                                                );

                                            }
                                        );
                                    }
                                );

                                scope.$on(
                                    "$destroy",
                                    function () {

                                        if (unwatch)
                                            unwatch();

                                        var managedClient =
                                            scope.client;

                                        if (!managedClient)
                                            return;

                                        managedClient.__espkvmMounted = false;

                                        Object.keys(
                                            api._frameClients
                                        ).forEach(
                                            function (token) {

                                                if (api._frameClients[token]
                                                        === managedClient) {
                                                    delete api._frameClients[token];
                                                }

                                            }
                                        );
                                    }
                                );
                            };

                            /*
                             * Preserve any existing Guacamole link function.
                             */
                            if (typeof originalLink === "function") {

                                return function () {

                                    originalLink.apply(
                                        this,
                                        arguments
                                    );

                                    espkvmPostLink(
                                        arguments[0],
                                        arguments[1]
                                    );
                                };
                            }

                            /*
                             * Also preserve directives which return separate
                             * pre/post link functions.
                             */
                            if (originalLink
                                    && typeof originalLink === "object") {

                                return {
                                    pre: originalLink.pre,

                                    post: function () {

                                        if (originalLink.post) {
                                            originalLink.post.apply(
                                                this,
                                                arguments
                                            );
                                        }

                                        espkvmPostLink(
                                            arguments[0],
                                            arguments[1]
                                        );
                                    }
                                };
                            }

                            return espkvmPostLink;
                        };

                        return $delegate;
                    }
                ]
            );
        }
    ]);




    /*
     * Generic HTTP transport for one specific ESPKVM ManagedClient.
     */
    api._httpRequestForClient = function (
            managedClient,
            method,
            path,
            body,
            contentType) {

        return new Promise(function (resolve, reject) {

            if (!managedClient || !managedClient.client) {
                reject(new Error("Invalid ESPKVM client."));
                return;
            }

            method = String(method || "GET").toUpperCase();

            if (!["GET", "POST", "PUT", "DELETE"].includes(method)) {
                reject(new Error("Unsupported HTTP method: " + method));
                return;
            }

            if (typeof path !== "string"
                    || !path.startsWith("/api/")) {
                reject(new Error("Only /api/... paths are allowed."));
                return;
            }

            api._httpCounter = (api._httpCounter || 0) + 1;
            api._httpPending = api._httpPending || {};

            var requestId = String(api._httpCounter);

            api._httpPending[requestId] = {
                resolve: resolve,
                reject: reject
            };

            var stream = managedClient.client.createPipeStream(
                "application/json",
                "espkvm:http2:" + requestId
            );

            var writer = new Guacamole.StringWriter(stream);

            writer.sendText(JSON.stringify({
                method: method,
                path: path,
                contentType: contentType || "",
                body: body == null ? "" : String(body)
            }));

            writer.sendEnd();

            setTimeout(function () {

                var pending = api._httpPending[requestId];

                if (!pending)
                    return;

                delete api._httpPending[requestId];

                pending.reject(
                    new Error(
                        "ESPKVM HTTP2 request "
                        + requestId
                        + " timed out."
                    )
                );

            }, 15000);

        });
    };


    api.httpRequest = function (
            method,
            path,
            body,
            contentType) {

        if (!api.activeClient)
            return Promise.reject(
                new Error("No active ESPKVM connection.")
            );

        return api._httpRequestForClient(
            api.activeClient,
            method,
            path,
            body,
            contentType
        );
    };




    /*
     * Browser <-> guacd transport for ESPKVM's /ws WebSocket.
     */
    api._controlWsCounter = 0;
    api._controlWsPending = {};
    api._controlWsChannels = {};


    api._acceptControlWsRx = function (
            stream,
            mimetype,
            name) {

        var match = name.match(
            /^espkvm:control-ws:([0-9]+):rx$/
        );

        if (!match)
            return;

        var id = match[1];

        var channel =
            api._controlWsPending[id];

        if (!channel)
            return;

        delete api._controlWsPending[id];

        var reader =
            new Guacamole.ArrayBufferReader(stream);

        channel.reader = reader;

        api._controlWsChannels[id] =
            channel;

        reader.ondata = function (buffer) {

            if (channel.handlers
                    && channel.handlers.ondata) {

                channel.handlers.ondata(buffer);
            }
        };

        reader.onend = function () {

            delete api._controlWsChannels[id];

            if (channel.handlers
                    && channel.handlers.onclose) {

                channel.handlers.onclose();
            }
        };

        channel.resolve(id);
    };


    api._frameControlWsOpen = function (
            token,
            handlers) {

        return new Promise(function (
                resolve,
                reject) {

            var managedClient =
                api._frameClients[token];

            if (!managedClient
                    || !managedClient.client) {

                reject(
                    new Error(
                        "Unknown ESPKVM frame token."
                    )
                );

                return;
            }

            api._controlWsCounter++;

            var id =
                String(api._controlWsCounter);

            var stream =
                managedClient.client.createPipeStream(
                    "application/octet-stream",
                    "espkvm:control-ws:" + id
                );

            var writer =
                new Guacamole.ArrayBufferWriter(
                    stream
                );

            var channel = {
                id: id,
                token: token,
                writer: writer,
                reader: null,
                handlers: handlers,
                resolve: resolve,
                reject: reject,
                closed: false
            };

            api._controlWsPending[id] =
                channel;

            /*
             * If guacd never sends the RX pipe, the actual ESPKVM
             * WebSocket handshake failed.
             */
            setTimeout(function () {

                if (!api._controlWsPending[id])
                    return;

                delete api._controlWsPending[id];

                try {
                    writer.sendEnd();
                }
                catch (e) {}

                reject(
                    new Error(
                        "ESPKVM /ws connection timed out."
                    )
                );

            }, 10000);
        });
    };


    api._frameControlWsSend = function (
            token,
            id,
            buffer) {

        var channel =
            api._controlWsChannels[String(id)];

        if (!channel
                || channel.token !== token
                || channel.closed) {

            throw new Error(
                "ESPKVM /ws is not open."
            );
        }

        channel.writer.sendData(buffer);
    };


    api._frameControlWsClose = function (
            token,
            id) {

        id = String(id);

        var channel =
            api._controlWsChannels[id]
            || api._controlWsPending[id];

        if (!channel
                || channel.token !== token
                || channel.closed) {

            return;
        }

        channel.closed = true;

        delete api._controlWsPending[id];

        try {
            channel.writer.sendEnd();
        }
        catch (e) {}
    };




    /*
     * ESPKVM native MJPEG /stream transport.
     *
     * guacd sends:
     *
     *   uint32 big-endian JPEG length
     *   JPEG bytes
     *
     * One complete JPEG is delivered to the iframe at a time.
     */
    api._mjpegCounter = 0;
    api._mjpegPending = {};
    api._mjpegChannels = {};


    api._acceptMjpegRx = function (
            stream,
            mimetype,
            name) {

        var match = name.match(
            /^espkvm:mjpeg-stream:([0-9]+):rx$/
        );

        if (!match)
            return;

        var id = match[1];

        var channel =
            api._mjpegPending[id];

        if (!channel)
            return;

        delete api._mjpegPending[id];

        var reader =
            new Guacamole.ArrayBufferReader(stream);

        channel.reader = reader;

        channel.lengthHeader =
            new Uint8Array(4);

        channel.lengthHeaderUsed = 0;
        channel.expectedLength = -1;
        channel.frame = null;
        channel.frameUsed = 0;

        api._mjpegChannels[id] =
            channel;


        reader.ondata = function (buffer) {

            var input =
                new Uint8Array(buffer);

            var offset = 0;

            while (offset < input.length) {

                if (channel.expectedLength < 0) {

                    while (offset < input.length
                            && channel.lengthHeaderUsed < 4) {

                        channel.lengthHeader[
                            channel.lengthHeaderUsed++
                        ] = input[offset++];
                    }

                    if (channel.lengthHeaderUsed < 4)
                        continue;

                    channel.expectedLength =
                        (
                            (
                                channel.lengthHeader[0] * 0x1000000
                            )
                            + (
                                channel.lengthHeader[1] << 16
                            )
                            + (
                                channel.lengthHeader[2] << 8
                            )
                            + channel.lengthHeader[3]
                        ) >>> 0;

                    channel.lengthHeaderUsed = 0;

                    if (channel.expectedLength === 0
                            || channel.expectedLength
                                > 32 * 1024 * 1024) {

                        console.error(
                            "[guacamole-espkvm] Invalid MJPEG frame length:",
                            channel.expectedLength
                        );

                        api._frameMjpegClose(
                            channel.token,
                            channel.id
                        );

                        return;
                    }

                    channel.frame =
                        new Uint8Array(
                            channel.expectedLength
                        );

                    channel.frameUsed = 0;
                }

                var remaining =
                    channel.expectedLength
                    - channel.frameUsed;

                var available =
                    input.length - offset;

                var count =
                    Math.min(
                        remaining,
                        available
                    );

                channel.frame.set(
                    input.subarray(
                        offset,
                        offset + count
                    ),
                    channel.frameUsed
                );

                channel.frameUsed += count;
                offset += count;

                if (channel.frameUsed
                        === channel.expectedLength) {

                    var complete =
                        channel.frame.buffer;

                    channel.expectedLength = -1;
                    channel.frame = null;
                    channel.frameUsed = 0;

                    if (channel.handlers
                            && channel.handlers.onframe) {

                        channel.handlers.onframe(
                            complete
                        );
                    }
                }
            }
        };


        reader.onend = function () {

            if (!channel.closed) {

                api._frameMjpegClose(
                    channel.token,
                    channel.id
                );
            }

            if (channel.handlers
                    && channel.handlers.onclose) {

                channel.handlers.onclose();
            }
        };


        channel.resolve(id);
    };


    api._frameMjpegOpen = function (
            token,
            handlers) {

        return new Promise(function (
                resolve,
                reject) {

            var managedClient =
                api._frameClients[token];

            if (!managedClient
                    || !managedClient.client) {

                reject(
                    new Error(
                        "Unknown ESPKVM frame token."
                    )
                );

                return;
            }

            api._mjpegCounter++;

            var id =
                String(api._mjpegCounter);

            var stream =
                managedClient.client.createPipeStream(
                    "application/octet-stream",
                    "espkvm:mjpeg-stream:" + id
                );

            var writer =
                new Guacamole.ArrayBufferWriter(
                    stream
                );

            var channel = {
                id: id,
                token: token,
                writer: writer,
                reader: null,
                handlers: handlers,
                resolve: resolve,
                reject: reject,
                closed: false
            };

            api._mjpegPending[id] =
                channel;

            setTimeout(function () {

                if (!api._mjpegPending[id])
                    return;

                delete api._mjpegPending[id];

                try {
                    writer.sendEnd();
                }
                catch (e) {}

                reject(
                    new Error(
                        "ESPKVM /stream connection timed out."
                    )
                );

            }, 10000);
        });
    };


    api._frameMjpegClose = function (
            token,
            id) {

        id = String(id);

        var channel =
            api._mjpegChannels[id]
            || api._mjpegPending[id];

        if (!channel
                || channel.token !== token
                || channel.closed) {

            return;
        }

        channel.closed = true;

        delete api._mjpegPending[id];
        delete api._mjpegChannels[id];

        try {
            channel.writer.sendEnd();
        }
        catch (e) {}
    };



    /*
     * ESPKVM /video transport.
     *
     * guacd sends:
     *
     *   uint32 big-endian message length
     *   complete ESPKVM WebSocket message
     *
     * The Guacamole protocol may split these bytes across many blob
     * instructions. Reassemble here before firing one WebSocket message event.
     */
    api._videoWsCounter = 0;
    api._videoWsPending = {};
    api._videoWsChannels = {};


    api._acceptVideoWsRx = function (
            stream,
            mimetype,
            name) {

        var match = name.match(
            /^espkvm:video-ws:([0-9]+):rx$/
        );

        if (!match)
            return;

        var id = match[1];

        var channel =
            api._videoWsPending[id];

        if (!channel)
            return;

        delete api._videoWsPending[id];

        var reader =
            new Guacamole.ArrayBufferReader(stream);

        channel.reader = reader;

        channel.lengthHeader =
            new Uint8Array(4);

        channel.lengthHeaderUsed = 0;
        channel.expectedLength = -1;
        channel.message = null;
        channel.messageUsed = 0;

        api._videoWsChannels[id] =
            channel;


        reader.ondata = function (buffer) {

            var input =
                new Uint8Array(buffer);

            var offset = 0;

            while (offset < input.length) {

                /*
                 * First collect the four-byte network-order message length.
                 */
                if (channel.expectedLength < 0) {

                    while (offset < input.length
                            && channel.lengthHeaderUsed < 4) {

                        channel.lengthHeader[
                            channel.lengthHeaderUsed++
                        ] = input[offset++];
                    }

                    if (channel.lengthHeaderUsed < 4)
                        continue;

                    channel.expectedLength =
                        (
                            (
                                channel.lengthHeader[0] * 0x1000000
                            )
                            + (
                                channel.lengthHeader[1] << 16
                            )
                            + (
                                channel.lengthHeader[2] << 8
                            )
                            + channel.lengthHeader[3]
                        ) >>> 0;

                    channel.lengthHeaderUsed = 0;

                    /*
                     * Same safety ceiling as the native plugin.
                     */
                    if (channel.expectedLength === 0
                            || channel.expectedLength
                                > 32 * 1024 * 1024) {

                        console.error(
                            "[guacamole-espkvm] Invalid /video message length:",
                            channel.expectedLength
                        );

                        api._frameVideoWsClose(
                            channel.token,
                            channel.id
                        );

                        return;
                    }

                    channel.message =
                        new Uint8Array(
                            channel.expectedLength
                        );

                    channel.messageUsed = 0;
                }

                var remaining =
                    channel.expectedLength
                    - channel.messageUsed;

                var available =
                    input.length - offset;

                var count =
                    Math.min(
                        remaining,
                        available
                    );

                channel.message.set(
                    input.subarray(
                        offset,
                        offset + count
                    ),
                    channel.messageUsed
                );

                channel.messageUsed += count;
                offset += count;

                /*
                 * Deliver exactly one ArrayBuffer per original ESPKVM
                 * WebSocket message.
                 */
                if (channel.messageUsed
                        === channel.expectedLength) {

                    var complete =
                        channel.message.buffer;

                    channel.expectedLength = -1;
                    channel.message = null;
                    channel.messageUsed = 0;

                    if (channel.handlers
                            && channel.handlers.ondata) {

                        channel.handlers.ondata(
                            complete
                        );
                    }
                }
            }
        };


        reader.onend = function () {

            /*
             * guacd ended the receive side because the ESPKVM /video
             * WebSocket closed. End the matching transmit pipe as well
             * so guacd can destroy the old channel before another
             * /video connection is opened.
             */
            if (!channel.closed) {

                api._frameVideoWsClose(
                    channel.token,
                    channel.id
                );
            }

            if (channel.handlers
                    && channel.handlers.onclose) {

                channel.handlers.onclose();
            }
        };


        channel.resolve(id);
    };


    api._frameVideoWsOpen = function (
            token,
            handlers) {

        return new Promise(function (
                resolve,
                reject) {

            var managedClient =
                api._frameClients[token];

            if (!managedClient
                    || !managedClient.client) {

                reject(
                    new Error(
                        "Unknown ESPKVM frame token."
                    )
                );

                return;
            }

            api._videoWsCounter++;

            var id =
                String(api._videoWsCounter);

            var stream =
                managedClient.client.createPipeStream(
                    "application/octet-stream",
                    "espkvm:video-ws:" + id
                );

            var writer =
                new Guacamole.ArrayBufferWriter(
                    stream
                );

            var channel = {
                id: id,
                token: token,
                writer: writer,
                reader: null,
                handlers: handlers,
                resolve: resolve,
                reject: reject,
                closed: false
            };

            api._videoWsPending[id] =
                channel;

            setTimeout(function () {

                if (!api._videoWsPending[id])
                    return;

                delete api._videoWsPending[id];

                try {
                    writer.sendEnd();
                }
                catch (e) {}

                reject(
                    new Error(
                        "ESPKVM /video connection timed out."
                    )
                );

            }, 10000);
        });
    };


    api._frameVideoWsSend = function (
            token,
            id,
            buffer) {

        var channel =
            api._videoWsChannels[
                String(id)
            ];

        if (!channel
                || channel.token !== token
                || channel.closed) {

            throw new Error(
                "ESPKVM /video is not open."
            );
        }

        channel.writer.sendData(buffer);
    };


    api._frameVideoWsClose = function (
            token,
            id) {

        id = String(id);

        var channel =
            api._videoWsChannels[id]
            || api._videoWsPending[id];

        if (!channel
                || channel.token !== token
                || channel.closed) {

            return;
        }

        channel.closed = true;

        delete api._videoWsPending[id];
        delete api._videoWsChannels[id];

        try {
            channel.writer.sendEnd();
        }
        catch (e) {}
    };


})();
