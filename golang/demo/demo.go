package main

import (
	"fmt"
	"log"
	"net/http"

	"github.com/progrium/duplex/golang"
	"golang.org/x/net/websocket"
)

var rpc = duplex.NewRPC(duplex.NewJSONCodec())

func init() {
	rpc.Register("echo", func(ch *duplex.Channel) error {
		var obj interface{}
		if _, err := ch.Recv(&obj); err != nil {
			return err
		}
		return ch.Send(obj, false)
	})
	rpc.Register("doMsgbox", func(ch *duplex.Channel) error {
		var text string
		if _, err := ch.Recv(&text); err != nil {
			return err
		}
		return ch.Call("msgbox", text, nil)
	})

}

func WebsocketServer(ws *websocket.Conn) {
	peer, err := rpc.Accept(ws)
	if err != nil {
		panic(err)
	}
	<-peer.CloseNotify()
	fmt.Println("Closed")
}

func main() {
	http.HandleFunc("/duplex.js", func(w http.ResponseWriter, r *http.Request) {
		http.ServeFile(w, r, "../../javascript/dist/duplex.js")
	})
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		http.ServeFile(w, r, "../../javascript/demo/index.html")
	})
	go func() {
		fmt.Println("HTTP on 8000...")
		log.Fatal(http.ListenAndServe(":8000", nil))
	}()

	ws := &http.Server{
		Addr:    ":8001",
		Handler: websocket.Handler(WebsocketServer),
	}
	fmt.Println("WS on 8001...")
	log.Fatal(ws.ListenAndServe())
}
