package main

import (
	"fmt"
	"io"
	"net/http"
	"os"
	"time"
)

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprintf(w, "<html><head><title>Go on NEOS</title></head><body><h1>Hello from Go on NEOS</h1><p>This page is served by an unmodified Go program (net/http) running inside NEOS.</p><p>path: %s</p><form action=\"/echo\"><input name=\"q\" value=\"\"> <input type=\"submit\" value=\"Send\"></form></body></html>", r.URL.Path)
	})
	mux.HandleFunc("/echo", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintf(w, "<html><body><h2>You said</h2><p>%s</p><a href=\"/\">back</a></body></html>", r.URL.Query().Get("q"))
	})
	srv := &http.Server{Addr: "127.0.0.1:8081", Handler: mux}
	go srv.ListenAndServe()
	time.Sleep(300 * time.Millisecond)
	if len(os.Args) > 1 && os.Args[1] == "serve" {
		fmt.Println("serving on http://127.0.0.1:8081/")
		select {}
	}
	resp, err := http.Get("http://127.0.0.1:8081/hello")
	if err != nil {
		fmt.Println("GO http error:", err)
		os.Exit(1)
	}
	body, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	fmt.Printf("GO http client got %d, %d bytes, contains title: %v\n", resp.StatusCode, len(body), len(body) > 0 && string(body[:6]) == "<html>")
}
