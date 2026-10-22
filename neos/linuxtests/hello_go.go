package main

import (
	"fmt"
	"os"
	"sync"
)

func main() {
	var wg sync.WaitGroup
	var mu sync.Mutex
	total := 0
	for i := 1; i <= 4; i++ {
		wg.Add(1)
		go func(n int) {
			defer wg.Done()
			s := 0
			for k := 0; k < 100000; k++ { s += k % (n + 1) }
			mu.Lock(); total += s; mu.Unlock()
		}(i)
	}
	wg.Wait()
	fmt.Println("hello from Go on NEOS, goroutines total =", total, "args =", len(os.Args))
}
