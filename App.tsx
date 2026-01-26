
import React, { useState, useEffect, useMemo, useRef } from 'react';
import { Product, ShoppingItem, ProductStatus, Origin, Shop, CartHistory, Recipe } from './types';
import { MOCK_PRODUCTS, MOCK_SHOPPING, CATEGORIES as DEFAULT_CATEGORIES, LOCATIONS as DEFAULT_LOCATIONS, MOCK_SHOPS } from './constants';
import ProductCard from './components/ProductCard';
import Assistant from './components/Assistant';
import { analyzeReceipt, parseVoiceCommand, analyzeProductImage, generateRecipeSuggestions, searchWebRecipes } from './services/geminiService';

type User = {
  email: string;
  name: string;
  photo?: string;
  method: 'google' | 'email';
};

const UNITS = ['Pezzi', 'Kg', 'gr', 'Litri', 'Confezioni'];

const App: React.FC = () => {
  const [currentUser, setCurrentUser] = useState<User | null>(null);
  const [authMode, setAuthMode] = useState<'login' | 'signup' | 'verify'>('login');
  const [authEmail, setAuthEmail] = useState('');
  const [authPassword, setAuthPassword] = useState('');
  const [verificationCode, setVerificationCode] = useState('');
  
  const [activeTab, setActiveTab] = useState<'home' | 'shopping' | 'chef' | 'stats' | 'settings' | 'history'>('home');
  const [chefSubTab, setChefSubTab] = useState<'ai' | 'web'>('ai');
  const [products, setProducts] = useState<Product[]>(MOCK_PRODUCTS);
  const [shoppingList, setShoppingList] = useState<ShoppingItem[]>(MOCK_SHOPPING);
  const [shops, setShops] = useState<Shop[]>(MOCK_SHOPS);
  const [history, setHistory] = useState<CartHistory[]>([]);
  
  const [categories, setCategories] = useState<string[]>(DEFAULT_CATEGORIES);
  const [locations, setLocations] = useState<string[]>(DEFAULT_LOCATIONS);
  
  const [isScanning, setIsScanning] = useState<'product' | 'receipt' | 'camera_only' | null>(null);
  const [isAddingProduct, setIsAddingProduct] = useState(false);
  const [isAddingToShopping, setIsAddingToShopping] = useState(false);
  const [isAnalyzingImage, setIsAnalyzingImage] = useState(false);
  const [productToDelete, setProductToDelete] = useState<Product | null>(null);
  const [editingProduct, setEditingProduct] = useState<Product | null>(null);
  const [editingShoppingItem, setEditingShoppingItem] = useState<ShoppingItem | null>(null);
  
  const [cookingProduct, setCookingProduct] = useState<Product | null>(null);
  const [newDishName, setNewDishName] = useState('');

  const [filterCategory, setFilterCategory] = useState('Tutti');
  const [filterLocation, setFilterLocation] = useState('Tutti');
  const [searchQuery, setSearchQuery] = useState('');

  const [selectedChefIngredients, setSelectedChefIngredients] = useState<string[]>([]);
  const [aiRecipes, setAiRecipes] = useState<Recipe[]>([]);
  const [webRecipes, setWebRecipes] = useState<Recipe[]>([]);
  const [isGeneratingRecipes, setIsGeneratingRecipes] = useState(false);
  const [selectedRecipe, setSelectedRecipe] = useState<Recipe | null>(null);

  const [isClosingCart, setIsClosingCart] = useState<string | null>(null);
  const [cartClosureStep, setCartClosureStep] = useState<'choice' | 'manual_price' | 'handle_remaining'>('choice');
  const [manualTotalPrice, setManualTotalPrice] = useState<string>('');
  const [moveRemainingTo, setMoveRemainingTo] = useState<string>('delete');

  const videoRef = useRef<HTMLVideoElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const streamRef = useRef<MediaStream | null>(null);

  const [newProductForm, setNewProductForm] = useState<{
    name: string;
    brand?: string;
    notes?: string;
    category?: string;
    location?: string;
    expiryDate?: string;
    quantity?: number;
    unit?: string;
    shopId?: string;
    expiryFoundInImage?: boolean;
    suggestedShopName?: string;
  }>({ 
    name: '', 
    brand: '', 
    notes: '', 
    category: 'Altro', 
    location: 'Frigo', 
    expiryDate: '', 
    quantity: 1, 
    unit: 'Pezzi', 
    shopId: '' 
  });

  const [newShoppingForm, setNewShoppingForm] = useState<{
    name: string;
    quantity?: number;
    unit?: string;
    shopId?: string;
    brand?: string;
    notes?: string;
  }>({ name: '', quantity: 1, unit: 'Pezzi', shopId: 'Tutti', brand: '', notes: '' });

  // Camera Management
  useEffect(() => {
    let stream: MediaStream | null = null;
    let mounted = true;

    const startCamera = async () => {
      if (isScanning) {
        try {
          stream = await navigator.mediaDevices.getUserMedia({ 
            video: { facingMode: 'environment' } 
          });
          
          if (!mounted) {
            stream.getTracks().forEach(track => track.stop());
            return;
          }

          streamRef.current = stream;
          if (videoRef.current) {
            videoRef.current.srcObject = stream;
            videoRef.current.onloadedmetadata = () => {
              if (videoRef.current && mounted) {
                videoRef.current.play().catch(e => console.error("Play error:", e));
              }
            };
          }
        } catch (err) {
          console.error("Camera access error:", err);
          if (mounted) {
            alert("Impossibile accedere alla fotocamera. Controlla i permessi.");
            setIsScanning(null);
          }
        }
      }
    };

    if (isScanning) {
      startCamera();
    }

    return () => {
      mounted = false;
      if (stream) {
        stream.getTracks().forEach(track => track.stop());
      }
      if (streamRef.current) {
        streamRef.current.getTracks().forEach(track => track.stop());
        streamRef.current = null;
      }
    };
  }, [isScanning]);

  // Authentication Handlers
  const handleGoogleLogin = () => {
    setCurrentUser({
      email: 'user.google@gmail.com',
      name: 'Google User',
      photo: 'https://api.dicebear.com/7.x/avataaars/svg?seed=Felix',
      method: 'google'
    });
  };

  const handleEmailAuth = (e: React.FormEvent) => {
    e.preventDefault();
    if (!authEmail || !authPassword) return alert("Inserisci email e password");
    if (authMode === 'signup') {
      setAuthMode('verify');
    } else {
      setCurrentUser({ email: authEmail, name: authEmail.split('@')[0], method: 'email' });
    }
  };

  const handleVerify = (e: React.FormEvent) => {
    e.preventDefault();
    if (verificationCode.length === 4) {
      setCurrentUser({ email: authEmail, name: authEmail.split('@')[0], method: 'email' });
    } else {
      alert("Codice non valido");
    }
  };

  const handleLogout = () => {
    setCurrentUser(null);
    setAuthMode('login');
    setAuthEmail('');
    setAuthPassword('');
  };

  const stats = useMemo(() => {
    const totalSpent = history.reduce((acc, cart) => acc + cart.totalSpent, 0);
    const shopSpending: Record<string, number> = {};
    const catSpending: Record<string, number> = {};
    
    history.forEach(cart => {
      const sName = shops.find(s => s.id === cart.shopId)?.name || 'Sconosciuto';
      shopSpending[sName] = (shopSpending[sName] || 0) + cart.totalSpent;
      cart.items.forEach(item => {
        catSpending[item.category] = (catSpending[item.category] || 0) + (item.price * item.quantity);
      });
    });

    return { totalSpent, shopSpending, catSpending };
  }, [history, shops]);

  const stockStats = useMemo(() => {
    return {
      total: products.length,
      expiring: products.filter(p => p.status === ProductStatus.EXPIRING).length,
      expired: products.filter(p => p.status === ProductStatus.EXPIRED).length,
      safe: products.filter(p => p.status === ProductStatus.SAFE).length,
    };
  }, [products]);

  const filteredProducts = useMemo(() => {
    return products.filter(p => {
      const matchCat = filterCategory === 'Tutti' || p.category === filterCategory;
      const matchLoc = filterLocation === 'Tutti' || p.location === filterLocation;
      const matchSearch = p.name.toLowerCase().includes(searchQuery.toLowerCase());
      return matchCat && matchLoc && matchSearch;
    });
  }, [products, filterCategory, filterLocation, searchQuery]);

  const suggestShopForProduct = (productName: string) => {
    const term = productName.toLowerCase();
    for (const cart of history) {
      for (const item of cart.items) {
        if (item.name.toLowerCase().includes(term)) return cart.shopId;
      }
    }
    return 'Tutti';
  };

  const handleUpdateQuantity = (id: string, newQty: number) => {
    if (newQty === 0) {
      const p = products.find(x => x.id === id);
      if (p) setProductToDelete(p);
    } else {
      setProducts(prev => prev.map(x => x.id === id ? { ...x, quantity: newQty } : x));
    }
  };

  const confirmDeleteAndAddShopping = (addToShopping: boolean) => {
    if (!productToDelete) return;
    if (addToShopping) {
      const shopId = suggestShopForProduct(productToDelete.name);
      setShoppingList(prev => [{
        id: Math.random().toString(36).substr(2, 9),
        name: productToDelete.name, 
        brand: productToDelete.brand,
        quantity: 1, 
        unit: productToDelete.unit,
        shopId: shopId, 
        isPurchased: false
      }, ...prev]);
    }
    setProducts(prev => prev.filter(p => p.id !== productToDelete.id));
    setProductToDelete(null);
  };

  const handleCookTransformation = () => {
    if (!cookingProduct || !newDishName) return;

    const newDish: Product = {
      id: Math.random().toString(36).substr(2, 9),
      name: newDishName,
      category: 'Cucinato',
      location: cookingProduct.location,
      quantity: 1,
      unit: 'porzione',
      expiryDate: new Date(Date.now() + 3 * 86400000).toISOString().split('T')[0],
      status: ProductStatus.SAFE,
      origin: Origin.AI,
      notes: `Cucinato a partire da ${cookingProduct.name}`
    };

    setProducts(prev => [newDish, ...prev.filter(p => p.id !== cookingProduct.id)]);
    setCookingProduct(null);
    setNewDishName('');
  };

  const finalizeCartClosure = (totalPrice?: number, scannedData?: any) => {
    const currentShopId = isClosingCart!;
    const itemsInCart = shoppingList.filter(i => i.shopId === currentShopId);
    const purchased = itemsInCart.filter(i => i.isPurchased);
    const remaining = itemsInCart.filter(i => !i.isPurchased);

    const finalTotal = scannedData 
      ? scannedData.items.reduce((acc: number, val: any) => acc + (val.pricePerUnit * val.quantity), 0) 
      : (totalPrice || purchased.length * 2.5);

    const newCart: CartHistory = {
      id: Math.random().toString(36).substr(2, 9),
      shopId: currentShopId,
      date: new Date().toISOString(),
      totalSpent: finalTotal,
      items: scannedData ? scannedData.items.map((item: any) => ({
        name: item.name, quantity: item.quantity, price: item.pricePerUnit,
        category: products.find(pr => pr.name === item.name)?.category || 'Altro'
      })) : purchased.map(p => ({
        name: p.name, quantity: p.quantity, price: finalTotal / (purchased.length || 1),
        category: products.find(pr => pr.name === p.name)?.category || 'Altro'
      }))
    };
    
    setHistory(prev => [newCart, ...prev]);

    const newInventoryEntries = newCart.items.map(item => ({
      id: Math.random().toString(36).substr(2, 9),
      name: item.name, category: item.category, location: 'Dispensa', quantity: item.quantity, unit: 'pz',
      expiryDate: new Date(Date.now() + 14 * 86400000).toISOString().split('T')[0],
      status: ProductStatus.SAFE, origin: scannedData ? Origin.RECEIPT : Origin.MANUAL,
      shop: shops.find(s => s.id === currentShopId)?.name, averagePrice: item.price
    }));
    
    setProducts(prev => [...prev, ...newInventoryEntries]);

    let finalShopping = shoppingList.filter(i => i.shopId !== currentShopId);
    if (moveRemainingTo !== 'delete') {
      const moved = remaining.map(r => ({
        ...r, shopId: moveRemainingTo === 'keep' ? currentShopId : moveRemainingTo, isPurchased: false
      }));
      finalShopping = [...finalShopping, ...moved];
    }
    setShoppingList(finalShopping);
    setIsClosingCart(null);
    setCartClosureStep('choice');
  };

  const handleSaveNewProduct = () => {
    if (!newProductForm.name || !newProductForm.expiryDate) {
      alert("Nome e Data di Scadenza sono obbligatori");
      return;
    }
    const product: Product = {
      id: Math.random().toString(36).substr(2, 9),
      name: newProductForm.name!, brand: newProductForm.brand, notes: newProductForm.notes,
      category: newProductForm.category || 'Altro', location: newProductForm.location || 'Frigo',
      quantity: newProductForm.quantity || 1, unit: newProductForm.unit || 'pz',
      expiryDate: newProductForm.expiryDate!, status: ProductStatus.SAFE, origin: Origin.MANUAL,
      shop: shops.find(s => s.id === newProductForm.shopId)?.name
    };
    setProducts(prev => [product, ...prev]);
    setIsAddingProduct(false);
    setNewProductForm({ name: '', brand: '', notes: '', category: 'Altro', location: 'Frigo', expiryDate: '', quantity: 1, unit: 'Pezzi', shopId: '' });
  };

  const handleSaveNewShopping = () => {
    if (!newShoppingForm.name) {
      alert("Il nome dell'articolo è obbligatorio");
      return;
    }
    const item: ShoppingItem = {
      id: Math.random().toString(36).substr(2, 9),
      name: newShoppingForm.name!, 
      brand: newShoppingForm.brand,
      quantity: newShoppingForm.quantity || 1,
      unit: newShoppingForm.unit || 'Pezzi', 
      shopId: newShoppingForm.shopId || 'Tutti', 
      isPurchased: false,
      notes: newShoppingForm.notes
    };
    setShoppingList(prev => [item, ...prev]);
    setIsAddingToShopping(false);
    setNewShoppingForm({ name: '', quantity: 1, unit: 'Pezzi', shopId: 'Tutti', brand: '', notes: '' });
  };

  const handleUpdateProduct = () => {
    if (!editingProduct) return;
    setProducts(prev => prev.map(p => p.id === editingProduct.id ? editingProduct : p));
    setEditingProduct(null);
  };

  const handleUpdateShoppingItem = () => {
    if (!editingShoppingItem) return;
    setShoppingList(prev => prev.map(item => item.id === editingShoppingItem.id ? editingShoppingItem : item));
    setEditingShoppingItem(null);
  };

  const handleChefAction = async () => {
    if (selectedChefIngredients.length === 0) return alert("Seleziona almeno un ingrediente!");
    setIsGeneratingRecipes(true);
    try {
      if (chefSubTab === 'ai') {
        const res = await generateRecipeSuggestions(selectedChefIngredients, products.map(p => p.name));
        setAiRecipes(res);
      } else {
        const res = await searchWebRecipes(selectedChefIngredients);
        setWebRecipes(res);
      }
    } catch (err) { alert("Errore generazione ricette"); } finally { setIsGeneratingRecipes(false); }
  };

  const captureAndScan = async () => {
    if (canvasRef.current && videoRef.current) {
      const ctx = canvasRef.current.getContext('2d');
      if (ctx && videoRef.current.readyState >= 2) {
        canvasRef.current.width = videoRef.current.videoWidth;
        canvasRef.current.height = videoRef.current.videoHeight;
        ctx.drawImage(videoRef.current, 0, 0, canvasRef.current.width, canvasRef.current.height);
        const dataUrl = canvasRef.current.toDataURL('image/jpeg', 0.8);
        const base64 = dataUrl.split(',')[1];
        
        setIsScanning(null);
        setIsAnalyzingImage(true);
        try {
          if (isClosingCart) {
            const data = await analyzeReceipt(base64);
            finalizeCartClosure(undefined, data);
          } else {
            const aiData = await analyzeProductImage(base64);
            setNewProductForm(prev => ({ ...prev, ...aiData, shopId: suggestShopForProduct(aiData.name) }));
            setIsAddingProduct(true);
          }
        } catch (e) { 
          console.error("AI Analysis error:", e);
          alert("Errore durante l'analisi AI. Riprova."); 
        } finally { 
          setIsAnalyzingImage(false); 
        }
      }
    }
  };

  const handleFileUpload = async (e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = async (event) => {
      const base64 = (event.target?.result as string).split(',')[1];
      setIsAnalyzingImage(true);
      try {
        if (isClosingCart) {
          const scanned = await analyzeReceipt(base64);
          finalizeCartClosure(undefined, scanned);
        } else {
          const aiData = await analyzeProductImage(base64);
          setNewProductForm(prev => ({ ...prev, ...aiData, shopId: suggestShopForProduct(aiData.name) }));
          setIsAddingProduct(true);
        }
      } catch (err) { alert("Errore AI"); } finally { setIsAnalyzingImage(false); }
    };
    reader.readAsDataURL(file);
  };

  if (!currentUser) {
    return (
      <div className="min-h-screen bg-[#0a0b10] flex flex-col items-center justify-center p-6 animate-in fade-in">
        <div className="w-full max-sm:px-4 max-w-sm space-y-8">
          <div className="text-center space-y-2">
            <div className="w-20 h-20 bg-emerald-500 rounded-3xl mx-auto flex items-center justify-center text-white shadow-2xl mb-4">
              <svg className="w-12 h-12" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M19 11H5m14 0a2 2 0 012 2v6a2 2 0 01-2 2H5a2 2 0 01-2-2v-6a2 2 0 012-2m14 0V9a2 2 0 00-2-2M5 11V9a2 2 0 012-2m0 0V5a2 2 0 012-2h6a2 2 0 012 2v2M7 7h10" /></svg>
            </div>
            <h1 className="text-4xl font-black tracking-tighter text-white">Fridge44</h1>
            <p className="text-emerald-500 font-bold uppercase tracking-widest text-[10px]">Smart AI Food Management</p>
          </div>

          <div className="glass p-8 rounded-[2rem] border border-white/10 space-y-6 shadow-2xl">
            {authMode === 'verify' ? (
              <form onSubmit={handleVerify} className="space-y-4">
                <h2 className="text-xl font-bold text-center">Verifica Email</h2>
                <p className="text-xs text-gray-500 text-center">Abbiamo inviato un codice a 4 cifre a {authEmail}</p>
                <input 
                  autoFocus
                  maxLength={4}
                  value={verificationCode}
                  onChange={e => setVerificationCode(e.target.value)}
                  className="w-full bg-white/5 border border-white/10 rounded-2xl py-4 text-center text-3xl font-black tracking-[1rem] focus:ring-2 focus:ring-emerald-500 outline-none"
                  placeholder="0000"
                />
                <button type="submit" className="w-full bg-emerald-500 py-4 rounded-2xl font-black uppercase text-xs tracking-widest text-white shadow-lg active:scale-95 transition-all">Verifica e Accedi</button>
                <button type="button" onClick={() => setAuthMode('signup')} className="w-full text-[10px] font-black uppercase text-gray-500 text-center">Torna indietro</button>
              </form>
            ) : (
              <>
                <button 
                  onClick={handleGoogleLogin}
                  className="w-full bg-white text-black py-4 rounded-2xl font-bold flex items-center justify-center gap-3 active:scale-95 transition-all shadow-xl"
                >
                  <svg className="w-5 h-5" viewBox="0 0 24 24"><path fill="#4285F4" d="M22.56 12.25c0-.78-.07-1.53-.2-2.25H12v4.26h5.92c-.26 1.37-1.04 2.53-2.21 3.31v2.77h3.57c2.08-1.92 3.28-4.74 3.28-8.09z"/><path fill="#34A853" d="M12 23c2.97 0 5.46-.98 7.28-2.66l-3.57-2.77c-.98.66-2.23 1.06-3.71 1.06-2.86 0-5.29-1.93-6.16-4.53H2.18v2.84C3.99 20.53 7.7 23 12 23z"/><path fill="#FBBC05" d="M5.84 14.09c-.22-.66-.35-1.36-.35-2.09s.13-1.43.35-2.09V7.07H2.18C1.43 8.55 1 10.22 1 12s.43 3.45 1.18 4.93l3.66-2.84z"/><path fill="#EA4335" d="M12 5.38c1.62 0 3.06.56 4.21 1.64l3.15-3.15C17.45 2.09 14.97 1 12 1 7.7 1 3.99 3.47 2.18 7.07l3.66 2.84c.87-2.6 3.3-4.53 6.16-4.53z"/></svg>
                  Continua con Google
                </button>

                <div className="relative">
                  <div className="absolute inset-0 flex items-center"><div className="w-full border-t border-white/10"></div></div>
                  <div className="relative flex justify-center text-[10px] uppercase font-black tracking-widest"><span className="bg-[#0f111a] px-2 text-gray-500">Oppure</span></div>
                </div>

                <form onSubmit={handleEmailAuth} className="space-y-4">
                  <input 
                    type="email" 
                    placeholder="Email" 
                    value={authEmail}
                    onChange={e => setAuthEmail(e.target.value)}
                    className="w-full bg-white/5 border border-white/10 rounded-2xl py-4 px-6 text-sm focus:ring-2 focus:ring-emerald-500 outline-none transition-all"
                  />
                  <input 
                    type="password" 
                    placeholder="Password" 
                    value={authPassword}
                    onChange={e => setAuthPassword(e.target.value)}
                    className="w-full bg-white/5 border border-white/10 rounded-2xl py-4 px-6 text-sm focus:ring-2 focus:ring-emerald-500 outline-none transition-all"
                  />
                  <button type="submit" className="w-full bg-indigo-600 py-4 rounded-2xl font-black uppercase text-xs tracking-widest text-white shadow-lg active:scale-95 transition-all">
                    {authMode === 'login' ? 'Accedi' : 'Registrati'}
                  </button>
                </form>

                <div className="text-center">
                  <button 
                    onClick={() => setAuthMode(authMode === 'login' ? 'signup' : 'login')}
                    className="text-[10px] font-black uppercase text-emerald-500 tracking-widest"
                  >
                    {authMode === 'login' ? 'Non hai un account? Registrati' : 'Hai già un account? Accedi'}
                  </button>
                </div>
              </>
            )}
          </div>
          <p className="text-center text-[10px] text-gray-600 font-bold uppercase tracking-tighter">Powered by Fridge44 AI Ecosystem</p>
        </div>
      </div>
    );
  }

  const ShoppingItemRow: React.FC<{ item: ShoppingItem }> = ({ item }) => (
    <div className={`flex items-center justify-between p-2 rounded-xl border ${item.isPurchased ? 'bg-emerald-500/5 border-emerald-500/10' : 'bg-white/5 border-white/5'}`}>
      <div className="flex items-center gap-3 flex-1">
        <button onClick={() => setShoppingList(s => s.map(i => i.id === item.id ? {...i, isPurchased: !i.isPurchased} : i))} className={`w-6 h-6 rounded-md border flex items-center justify-center transition-all ${item.isPurchased ? 'bg-emerald-500 border-emerald-500' : 'border-white/10'}`}>
          {item.isPurchased && <svg className="w-3.5 h-3.5 text-white" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M5 13l4 4L19 7" /></svg>}
        </button>
        <div>
          <p className={`text-xs font-bold ${item.isPurchased ? 'line-through text-gray-500' : 'text-white'}`}>{item.name}</p>
          <div className="flex items-center gap-2">
            <span className="text-[9px] text-gray-500">{item.quantity} {item.unit}</span>
            <select value={item.shopId} onChange={(e) => setShoppingList(s => s.map(i => i.id === item.id ? {...i, shopId: e.target.value} : i))} className="bg-transparent text-[8px] font-black text-indigo-400 uppercase outline-none border-none">
              <option value="Tutti">Sposta...</option>
              {shops.map(s => <option key={s.id} value={s.id}>{s.name}</option>)}
            </select>
          </div>
        </div>
      </div>
      <div className="flex items-center gap-1">
         <button onClick={() => setEditingShoppingItem(item)} className="p-1.5 text-gray-600 hover:text-white"><svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M11 5H6a2 2 0 00-2 2v11a2 2 0 002 2h11a2 2 0 002-2v-5m-1.414-9.414a2 2 0 112.828 2.828L11.828 15H9v-2.828l8.586-8.586z" /></svg></button>
         <button onClick={() => setShoppingList(s => s.filter(i => i.id !== item.id))} className="p-1.5 text-gray-600 hover:text-rose-500"><svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M6 18L18 6M6 6l12 12" /></svg></button>
      </div>
    </div>
  );

  const ShoppingListView = () => {
    type GroupData = { shopName: string; toBuy: ShoppingItem[]; purchased: ShoppingItem[] };
    const groupedItems = useMemo<Record<string, GroupData>>(() => {
      const groups: Record<string, GroupData> = {};
      shops.forEach(s => { groups[s.id] = { shopName: s.name, toBuy: [], purchased: [] }; });
      groups['Tutti'] = { shopName: '🛒 Carrello Generico', toBuy: [], purchased: [] };
      shoppingList.forEach(item => {
        const groupId = item.shopId || 'Tutti';
        if (!groups[groupId]) groups[groupId] = { shopName: 'Shop Sconosciuto', toBuy: [], purchased: [] };
        if (item.isPurchased) groups[groupId].purchased.push(item);
        else groups[groupId].toBuy.push(item);
      });
      return groups;
    }, [shoppingList, shops]);

    return (
      <div className="space-y-6 animate-in fade-in">
        <div className="flex items-center justify-between">
          <h2 className="text-xl font-bold">Spesa Intelligente</h2>
          <div className="flex gap-2">
            <button onClick={() => setShoppingList(s => s.map(i => ({...i, shopId: i.shopId === 'Tutti' ? suggestShopForProduct(i.name) : i.shopId})))} className="px-2.5 py-1.5 bg-indigo-500/20 text-indigo-400 rounded-lg text-[9px] font-black uppercase tracking-widest border border-indigo-500/30">AI Assegna</button>
            <button onClick={() => setIsAddingToShopping(true)} className="p-1.5 bg-emerald-500 rounded-lg text-white shadow-md"><svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M12 4v16m8-8H4" /></svg></button>
          </div>
        </div>
        {Object.entries(groupedItems).map(([shopId, group]) => {
          if (group.toBuy.length === 0 && group.purchased.length === 0) return null;
          return (
            <div key={shopId} className="bg-[#151821] rounded-2xl p-4 border border-white/5 space-y-3">
              <div className="flex items-center justify-between border-b border-white/5 pb-2">
                <h3 className="font-bold text-sm">{group.shopName}</h3>
                {shopId !== 'Tutti' && (
                  <button onClick={() => { setIsClosingCart(shopId); setCartClosureStep('choice'); }} className="px-2 py-1 bg-emerald-500 text-white rounded-lg text-[9px] font-black uppercase tracking-widest">Chiudi</button>
                )}
              </div>
              <div className="space-y-1.5">
                {group.toBuy.map(item => (
                  <ShoppingItemRow key={item.id} item={item} />
                ))}
              </div>
              {group.purchased.length > 0 && (
                <div className="space-y-1.5 mt-3 pt-3 border-t border-white/5 border-dashed opacity-60">
                  {group.purchased.map(item => (
                    <ShoppingItemRow key={item.id} item={item} />
                  ))}
                </div>
              )}
            </div>
          );
        })}
      </div>
    );
  };

  return (
    <div className="max-w-xl mx-auto min-h-screen bg-[#0a0b10] flex flex-col relative pb-32 overflow-x-hidden">
      
      <header className="px-4 pt-6 pb-4 sticky top-0 bg-[#0a0b10]/95 backdrop-blur-xl z-40 border-b border-white/5">
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-3">
            <div className="w-9 h-9 bg-emerald-500 rounded-xl flex items-center justify-center text-white shadow-lg">
              <svg className="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M19 11H5m14 0a2 2 0 012 2v6a2 2 0 01-2 2H5a2 2 0 01-2-2v-6a2 2 0 012-2m14 0V9a2 2 0 00-2-2M5 11V9a2 2 0 012-2m0 0V5a2 2 0 012-2h6a2 2 0 012 2v2M7 7h10" /></svg>
            </div>
            <div>
              <h1 className="text-lg font-bold tracking-tight">Fridge44</h1>
              <p className="text-[8px] text-emerald-500 font-black uppercase tracking-widest">Smart AI Manager</p>
            </div>
          </div>
          <div className="flex gap-2">
            <button onClick={() => setActiveTab('history')} className={`p-2 glass rounded-lg ${activeTab === 'history' ? 'text-indigo-400 bg-indigo-500/10' : 'text-gray-400'}`}>
               <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M12 8v4l3 3m6-3a9 9 0 11-18 0 9 9 0 0118 0z" /></svg>
            </button>
            <button onClick={() => setActiveTab('stats')} className="p-2 glass rounded-lg text-gray-400"><svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M9 19v-6a2 2 0 00-2-2H5a2 2 0 00-2 2v6a2 2 0 002 2h2a2 2 0 002-2zm0 0V9a2 2 0 012-2h2a2 2 0 012 2v10m-6 0a2 2 0 002 2h2a2 2 0 002-2m0 0V5a2 2 0 012-2h2a2 2 0 012 2v14a2 2 0 01-2 2h-2a2 2 0 01-2-2z" /></svg></button>
          </div>
        </div>
      </header>

      <main className="flex-1 px-4 space-y-4 overflow-y-auto pt-4 pb-10">
        
        {activeTab === 'home' && (
          <div className="space-y-6 animate-in fade-in">
            {/* Statistics Cards - New Dashboard Section */}
            <div className="grid grid-cols-2 gap-3">
              <div className="bg-[#151821] p-4 rounded-2xl border border-white/5 flex items-center justify-between">
                <div>
                  <p className="text-[24px] font-black text-white leading-none">{stockStats.total}</p>
                  <p className="text-[10px] text-gray-500 uppercase font-black tracking-widest mt-1">total</p>
                </div>
                <div className="w-10 h-10 bg-indigo-500/10 rounded-xl flex items-center justify-center text-indigo-400">
                  <svg className="w-6 h-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M20 7l-8-4-8 4m16 0l-8 4m8-4v10l-8 4m0-10L4 7m8 4v10M4 7v10l8 4" /></svg>
                </div>
              </div>
              <div className="bg-[#151821] p-4 rounded-2xl border border-white/5 flex items-center justify-between">
                <div>
                  <p className="text-[24px] font-black text-amber-500 leading-none">{stockStats.expiring}</p>
                  <p className="text-[10px] text-amber-500/60 uppercase font-black tracking-widest mt-1">expiring</p>
                </div>
                <div className="w-10 h-10 bg-amber-500/10 rounded-xl flex items-center justify-center text-amber-400">
                  <svg className="w-6 h-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M12 8v4l3 3m6-3a9 9 0 11-18 0 9 9 0 0118 0z" /></svg>
                </div>
              </div>
              <div className="bg-[#151821] p-4 rounded-2xl border border-white/5 flex items-center justify-between">
                <div>
                  <p className="text-[24px] font-black text-rose-500 leading-none">{stockStats.expired}</p>
                  <p className="text-[10px] text-rose-500/60 uppercase font-black tracking-widest mt-1">Scaduto</p>
                </div>
                <div className="w-10 h-10 bg-rose-500/10 rounded-xl flex items-center justify-center text-rose-400">
                  <svg className="w-6 h-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" /></svg>
                </div>
              </div>
              <div className="bg-[#151821] p-4 rounded-2xl border border-white/5 flex items-center justify-between">
                <div>
                  <p className="text-[24px] font-black text-emerald-500 leading-none">{stockStats.safe}</p>
                  <p className="text-[10px] text-emerald-500/60 uppercase font-black tracking-widest mt-1">safe</p>
                </div>
                <div className="w-10 h-10 bg-emerald-500/10 rounded-xl flex items-center justify-center text-emerald-400">
                  <svg className="w-6 h-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M9 12l2 2 4-4m6 2a9 9 0 11-18 0 9 9 0 0118 0z" /></svg>
                </div>
              </div>
            </div>

            {/* Search Bar */}
            <div className="relative">
              <input type="text" placeholder="Cerca prodotti..." value={searchQuery} onChange={(e) => setSearchQuery(e.target.value)}
                className="w-full bg-[#151821] border border-white/10 rounded-xl py-3.5 px-10 text-sm focus:ring-1 focus:ring-emerald-500 outline-none placeholder:text-gray-600" />
              <svg className="absolute left-3 top-1/2 -translate-y-1/2 w-4 h-4 text-gray-600" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M21 21l-6-6m2-5a7 7 0 11-14 0 7 7 0 0114 0z" /></svg>
            </div>

            {/* Filters Section */}
            <div className="space-y-4">
              <div>
                <h3 className="text-[11px] font-black uppercase text-gray-500 tracking-widest mb-2 px-1">Posizione</h3>
                <div className="flex gap-2 overflow-x-auto no-scrollbar pb-1">
                  {['Tutti', ...locations].map(loc => (
                    <button 
                      key={loc} 
                      onClick={() => setFilterLocation(loc)} 
                      className={`px-4 py-2 rounded-xl text-xs font-bold whitespace-nowrap transition-all flex items-center gap-2 ${filterLocation === loc ? 'bg-indigo-500 text-white shadow-lg' : 'bg-[#151821] text-gray-400 border border-white/5'}`}
                    >
                      {loc === 'Tutti' && <svg className="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M4 6h16M4 12h16m-7 6h7" /></svg>}
                      {loc === 'Frigo' && <svg className="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M3 12l2-2m0 0l7-7 7 7M5 10v10a1 1 0 001 1h3m10-11l2 2m-2-2v10a1 1 0 01-1 1h-3m-6 0a1 1 0 001-1v-4a1 1 0 011-1h2a1 1 0 011 1v4a1 1 0 001 1m-6 0h6" /></svg>}
                      {loc === 'Freezer' && <svg className="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M12 3v1m0 16v1m9-9h-1M4 12H3m15.364 6.364l-.707-.707M6.343 6.343l-.707-.707m12.728 0l-.707.707M6.343 17.657l-.707.707M16 12a4 4 0 11-8 0 4 4 0 018 0z" /></svg>}
                      {loc === 'Dispensa' && <svg className="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M20 7l-8-4-8 4m16 0l-8 4m8-4v10l-8 4m0-10L4 7m8 4v10M4 7v10l8 4" /></svg>}
                      {loc}
                    </button>
                  ))}
                </div>
              </div>

              <div>
                <h3 className="text-[11px] font-black uppercase text-gray-500 tracking-widest mb-2 px-1">Categoria</h3>
                <div className="flex gap-2 overflow-x-auto no-scrollbar pb-1">
                  {['Tutti', ...categories].map(cat => (
                    <button 
                      key={cat} 
                      onClick={() => setFilterCategory(cat)} 
                      className={`px-4 py-2 rounded-xl text-xs font-bold whitespace-nowrap transition-all flex items-center gap-2 ${filterCategory === cat ? 'bg-emerald-500 text-white shadow-lg' : 'bg-[#151821] text-gray-400 border border-white/5'}`}
                    >
                      {cat === 'Tutti' && <svg className="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M4 6h16M4 12h16m-7 6h7" /></svg>}
                      {cat}
                    </button>
                  ))}
                </div>
              </div>
            </div>

            <div className="flex items-center justify-between">
               <h2 className="text-xs font-black uppercase tracking-widest text-gray-500">Stock Corrente</h2>
               <button onClick={() => setIsAddingProduct(true)} className="p-1.5 bg-emerald-500 rounded-lg text-white shadow-md active:scale-90 transition-all"><svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M12 4v16m8-8H4" /></svg></button>
            </div>

            <div className="grid gap-2.5">
              {filteredProducts.length > 0 ? (
                filteredProducts.map(p => (
                  <ProductCard key={p.id} product={p} onCook={(prod) => setCookingProduct(prod)} onUpdateQuantity={handleUpdateQuantity} onDelete={() => setProductToDelete(p)} onEdit={(prod) => setEditingProduct(prod)} />
                ))
              ) : (
                <div className="py-12 flex flex-col items-center justify-center opacity-30 text-center">
                  <svg className="w-12 h-12 mb-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M21 21l-6-6m2-5a7 7 0 11-14 0 7 7 0 0114 0z" /></svg>
                  <p className="text-sm font-bold">Nessun prodotto trovato</p>
                </div>
              )}
            </div>
          </div>
        )}

        {activeTab === 'shopping' && <ShoppingListView />}

        {activeTab === 'chef' && (
          <div className="space-y-4 animate-in slide-in-from-bottom-2">
            <div className="flex items-center justify-between mb-2">
              <h2 className="text-xl font-bold">SmartChef AI</h2>
              <div className="flex glass rounded-xl p-1 scale-90">
                <button onClick={() => setChefSubTab('ai')} className={`px-3 py-1.5 rounded-lg text-[9px] font-black uppercase tracking-widest transition-all ${chefSubTab === 'ai' ? 'bg-emerald-500 text-white' : 'text-gray-500'}`}>Ricette AI</button>
                <button onClick={() => setChefSubTab('web')} className={`px-3 py-1.5 rounded-lg text-[9px] font-black uppercase tracking-widest transition-all ${chefSubTab === 'web' ? 'bg-emerald-500 text-white' : 'text-gray-500'}`}>Web & YT</button>
              </div>
            </div>
            <div className="space-y-3">
              <h3 className="text-[10px] font-black uppercase tracking-widest text-gray-500">Seleziona Ingredienti</h3>
              <div className="flex flex-wrap gap-1">
                {products.map(p => {
                  const isExpired = p.status === ProductStatus.EXPIRED;
                  const isExpiring = p.status === ProductStatus.EXPIRING;
                  const glowClass = isExpired ? 'animate-glow-blink-rose border-rose-500/50' : (isExpiring ? 'animate-glow-blink border-amber-500/50' : '');
                  
                  return (
                    <button 
                      key={p.id} 
                      onClick={() => setSelectedChefIngredients(prev => prev.includes(p.name) ? prev.filter(n => n !== p.name) : [...prev, p.name])} 
                      className={`px-2 py-1 rounded-lg border text-[10px] font-bold transition-all ${glowClass} ${selectedChefIngredients.includes(p.name) ? 'bg-emerald-500 text-white border-emerald-500' : 'bg-white/5 text-gray-400 border-white/5'}`}
                    >
                      {p.name}
                    </button>
                  );
                })}
              </div>
              <button onClick={handleChefAction} disabled={isGeneratingRecipes || selectedChefIngredients.length === 0} className="w-full py-3.5 bg-emerald-500 text-white rounded-xl font-black uppercase text-xs shadow-lg active:scale-95 transition-all disabled:opacity-50">
                {isGeneratingRecipes ? 'Generazione...' : 'Crea Ricette Smart'}
              </button>
            </div>
            <div className="grid gap-2.5 pt-4">
              {(chefSubTab === 'ai' ? aiRecipes : webRecipes).map((r, i) => {
                const total = r.ingredients?.length || 1;
                const missing = r.missingIngredients?.length || 0;
                const matchPercent = Math.round(((total - missing) / total) * 100);

                return (
                  <div key={i} onClick={() => setSelectedRecipe(r)} className="bg-[#151821] p-4 rounded-2xl border border-white/5 hover:border-emerald-500/50 transition-all cursor-pointer shadow-md group">
                     <div className="flex justify-between items-start mb-2">
                       <div className="flex gap-1.5">
                         <span className="text-[8px] font-black uppercase px-2 py-0.5 bg-indigo-500/20 text-indigo-400 rounded-lg">{r.difficulty}</span>
                         <span className={`text-[8px] font-black uppercase px-2 py-0.5 rounded-lg ${matchPercent > 70 ? 'bg-emerald-500/20 text-emerald-400' : 'bg-amber-500/20 text-amber-400'}`}>{matchPercent}% Match</span>
                         {r.youtubeId && <span className="text-[8px] font-black uppercase px-2 py-0.5 bg-rose-500/20 text-rose-400 rounded-lg">Video YT</span>}
                       </div>
                       <span className="text-[8px] font-black uppercase text-gray-500">{r.time} min</span>
                     </div>
                     <h3 className="font-bold text-white text-sm mb-1 group-hover:text-emerald-400 transition-colors">{r.title}</h3>
                     <p className="text-[11px] text-gray-500 line-clamp-2">{r.description}</p>
                  </div>
                );
              })}
            </div>
          </div>
        )}

        {activeTab === 'history' && (
          <div className="space-y-4 animate-in fade-in">
            <h2 className="text-xl font-bold">Storico Carrelli</h2>
            {history.length === 0 ? (
              <div className="flex flex-col items-center justify-center py-20 opacity-30">
                <svg className="w-16 h-16 mb-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="1" d="M19 11H5m14 0a2 2 0 012 2v6a2 2 0 01-2 2H5a2 2 0 01-2-2v-6a2 2 0 012-2m14 0V9a2 2 0 00-2-2M5 11V9a2 2 0 00-2-2M5 11V9a2 2 0 012-2m0 0V5a2 2 0 012-2h6a2 2 0 012 2v2M7 7h10" /></svg>
                <p className="text-sm font-bold">Nessun carrello archiviato</p>
              </div>
            ) : (
              history.map(cart => (
                <div key={cart.id} className="bg-[#151821] rounded-xl p-4 border border-white/5 shadow-md">
                  <div className="flex justify-between items-start mb-2">
                    <div>
                      <h3 className="font-bold text-sm">{shops.find(s => s.id === cart.shopId)?.name || 'Negozio'}</h3>
                      <p className="text-[10px] text-gray-500">{new Date(cart.date).toLocaleDateString()}</p>
                    </div>
                    <div className="text-emerald-400 font-black text-sm">€{cart.totalSpent.toFixed(2)}</div>
                  </div>
                  <div className="text-[10px] text-gray-500 italic">{cart.items.length} articoli acquistati</div>
                </div>
              ))
            )}
          </div>
        )}

        {activeTab === 'stats' && (
          <div className="space-y-6">
             <h2 className="text-xl font-bold">Statistiche</h2>
             <div className="grid grid-cols-2 gap-4">
               <div className="glass p-5 rounded-3xl border-l-4 border-emerald-500 shadow-xl">
                  <p className="text-[9px] font-black uppercase text-gray-500 tracking-widest mb-1">Totale Speso</p>
                  <p className="text-2xl font-black text-white">€{stats.totalSpent.toFixed(2)}</p>
               </div>
               <div className="glass p-5 rounded-3xl border-l-4 border-indigo-500 shadow-xl">
                  <p className="text-[9px] font-black uppercase text-gray-500 tracking-widest mb-1">Stima Mese</p>
                  <p className="text-2xl font-black text-white">€{(stats.totalSpent * 1.5).toFixed(2)}</p>
               </div>
             </div>
          </div>
        )}

        {activeTab === 'settings' && (
          <div className="space-y-8 animate-in fade-in">
            <div className="flex items-center justify-between">
              <h2 className="text-xl font-bold">Impostazioni</h2>
              <button 
                onClick={handleLogout}
                className="px-4 py-2 bg-rose-500/10 text-rose-500 border border-rose-500/20 rounded-xl text-[10px] font-black uppercase tracking-widest"
              >
                Logout
              </button>
            </div>

            <div className="glass p-6 rounded-3xl border border-white/5 flex items-center gap-4">
              <div className="w-16 h-16 rounded-2xl bg-indigo-500/10 flex items-center justify-center overflow-hidden">
                {currentUser.photo ? <img src={currentUser.photo} className="w-full h-full" /> : <span className="text-2xl font-black text-indigo-400">{currentUser.name[0]}</span>}
              </div>
              <div>
                <p className="font-bold text-white">{currentUser.name}</p>
                <p className="text-xs text-gray-500">{currentUser.email}</p>
                <span className="text-[8px] font-black uppercase px-2 py-0.5 bg-white/5 rounded mt-1 inline-block">Account {currentUser.method}</span>
              </div>
            </div>
            
            <section className="space-y-4">
              <div className="flex items-center justify-between">
                <h3 className="text-xs font-black uppercase tracking-widest text-gray-500">Shops</h3>
                <button onClick={() => {
                  const name = prompt("Nome nuovo negozio:");
                  if (name) setShops([...shops, { id: Math.random().toString(36).substr(2, 9), name }]);
                }} className="p-1.5 glass rounded-lg text-emerald-500"><svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M12 4v16m8-8H4" /></svg></button>
              </div>
              <div className="grid gap-2">
                {shops.map(s => (
                  <div key={s.id} className="flex items-center justify-between glass p-3 rounded-xl border border-white/5">
                    <span className="text-sm font-bold">{s.name}</span>
                    <button onClick={() => setShops(shops.filter(x => x.id !== s.id))} className="text-rose-500 p-1 hover:bg-rose-500/10 rounded-lg"><svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" /></svg></button>
                  </div>
                ))}
              </div>
            </section>
          </div>
        )}
      </main>

      {/* Navigation Bar */}
      <nav className="fixed bottom-0 left-0 right-0 h-16 bg-[#0a0b10]/95 border-t border-white/5 flex items-center justify-around px-2 z-[60] max-w-xl mx-auto backdrop-blur-xl">
        <NavBtn active={activeTab === 'home'} onClick={() => setActiveTab('home')} label="Stock" icon={<svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M3 12l2-2m0 0l7-7 7 7M5 10v10a1 1 0 001 1h3m10-11l2 2m-2-2v10a1 1 0 01-1 1h-3m-6 0a1 1 0 001-1v-4a1 1 0 011-1h2a1 1 0 011 1v4a1 1 0 001 1m-6 0h6" /></svg>} />
        <NavBtn active={activeTab === 'shopping'} onClick={() => setActiveTab('shopping')} label="Spesa" icon={<svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M16 11V7a4 4 0 00-8 0v4M5 9h14l1 12H4L5 9z" /></svg>} />
        <NavBtn active={activeTab === 'chef'} onClick={() => setActiveTab('chef')} label="Chef" icon={<svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M13 10V3L4 14h7v7l9-11h-7z"/></svg>} />
        <NavBtn active={activeTab === 'history'} onClick={() => setActiveTab('history')} label="Storico" icon={<svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M12 8v4l3 3m6-3a9 9 0 11-18 0 9 9 0 0118 0z" /></svg>} />
        <NavBtn active={activeTab === 'settings'} onClick={() => setActiveTab('settings')} label="Opzioni" icon={<svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.065 2.572c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.572 1.065c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.065-2.572c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z" /><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z" /></svg>} />
      </nav>

      {/* MODALS */}
      
      {/* Product Form Modal (Add) */}
      {isAddingProduct && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-sm z-[100] flex items-end sm:items-center justify-center p-4">
          <div className="bg-[#151821] w-full max-w-sm rounded-3xl p-5 border border-white/10 space-y-3 animate-in slide-in-from-bottom-10">
            <h3 className="text-lg font-bold">Nuovo Prodotto</h3>
            
            {/* Name */}
            <div>
               <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Nome Prodotto</label>
               <input 
                  placeholder="Es. Latte, Uova..." 
                  value={newProductForm.name} 
                  onChange={e => setNewProductForm({...newProductForm, name: e.target.value})}
                  className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none focus:border-emerald-500 text-sm transition-colors"
               />
            </div>

            {/* Brand */}
            <div>
               <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Marca (Opzionale)</label>
               <input 
                  placeholder="Es. Barilla..." 
                  value={newProductForm.brand || ''} 
                  onChange={e => setNewProductForm({...newProductForm, brand: e.target.value})}
                  className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none text-sm transition-colors"
               />
            </div>

            {/* Cat & Loc */}
            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Categoria</label>
                <select 
                  value={newProductForm.category}
                  onChange={e => setNewProductForm({...newProductForm, category: e.target.value})}
                  className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none text-sm appearance-none transition-colors"
                >
                  {categories.map(c => <option key={c} value={c}>{c}</option>)}
                </select>
              </div>
              <div>
                <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Posizione</label>
                <select 
                  value={newProductForm.location}
                  onChange={e => setNewProductForm({...newProductForm, location: e.target.value})}
                  className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none text-sm appearance-none transition-colors"
                >
                  {locations.map(l => <option key={l} value={l}>{l}</option>)}
                </select>
              </div>
            </div>

            {/* Qty, Unit, Expiry */}
            <div className="grid grid-cols-2 gap-3">
               <div>
                  <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Quantità</label>
                  <div className="flex gap-2">
                    <input 
                      type="number" 
                      value={newProductForm.quantity}
                      onChange={e => setNewProductForm({...newProductForm, quantity: Number(e.target.value)})}
                      className="w-16 bg-white/5 border border-white/10 rounded-xl px-2 py-2.5 outline-none text-center text-sm transition-colors"
                    />
                    <select 
                      value={newProductForm.unit}
                      onChange={e => setNewProductForm({...newProductForm, unit: e.target.value})}
                      className="flex-1 bg-white/5 border border-white/10 rounded-xl px-2 py-2.5 outline-none text-sm appearance-none transition-colors"
                    >
                      {UNITS.map(u => <option key={u} value={u}>{u}</option>)}
                    </select>
                  </div>
               </div>
               <div>
                  <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Scadenza</label>
                  <input 
                    type="date" 
                    value={newProductForm.expiryDate}
                    onChange={e => setNewProductForm({...newProductForm, expiryDate: e.target.value})}
                    className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none text-gray-400 text-sm transition-colors"
                  />
               </div>
            </div>

            {/* Shop & Notes */}
            <div>
               <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Dettagli Aggiuntivi</label>
                <div className="space-y-2">
                  <select 
                    value={newProductForm.shopId || ''}
                    onChange={e => setNewProductForm({...newProductForm, shopId: e.target.value})}
                    className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none text-sm text-gray-400 appearance-none transition-colors"
                  >
                    <option value="">Seleziona Negozio (opzionale)</option>
                    {shops.map(s => <option key={s.id} value={s.id}>{s.name}</option>)}
                  </select>
                  <textarea 
                    placeholder="Note / Descrizione conservazione..."
                    value={newProductForm.notes || ''}
                    onChange={e => setNewProductForm({...newProductForm, notes: e.target.value})}
                    className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none text-sm h-16 resize-none transition-colors"
                  />
                </div>
            </div>

            {/* Buttons */}
            <div className="flex gap-2 pt-2">
               <input type="file" ref={fileInputRef} className="hidden" accept="image/*" onChange={handleFileUpload} />
               <button onClick={() => setIsScanning('product')} className="flex-1 py-3 bg-white/5 hover:bg-white/10 rounded-xl font-bold text-xs uppercase tracking-wider flex items-center justify-center gap-2 transition-colors border border-white/5">
                 <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M3 9a2 2 0 012-2h.93a2 2 0 001.664-.89l.812-1.22A2 2 0 0110.07 4h3.86a2 2 0 011.664.89l.812 1.22A2 2 0 0018.07 7H19a2 2 0 012 2v9a2 2 0 01-2 2H5a2 2 0 01-2-2V9z" /><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M15 13a3 3 0 11-6 0 3 3 0 016 0z" /></svg>
                 Scan
               </button>
               <button onClick={() => fileInputRef.current?.click()} className="px-4 py-3 bg-white/5 hover:bg-white/10 rounded-xl font-bold text-xs uppercase tracking-wider flex items-center justify-center gap-2 transition-colors border border-white/5">
                  <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M4 16l4.586-4.586a2 2 0 012.828 0L16 16m-2-2l1.586-1.586a2 2 0 012.828 0L20 14m-6-6h.01M6 20h12a2 2 0 002-2V6a2 2 0 00-2-2H6a2 2 0 00-2 2v12a2 2 0 002 2z" /></svg>
               </button>
              <button onClick={handleSaveNewProduct} className="flex-[2] py-3 bg-emerald-500 hover:bg-emerald-600 rounded-xl font-bold text-xs uppercase tracking-wider text-white shadow-lg shadow-emerald-500/20 transition-all">Salva</button>
            </div>
            <button onClick={() => setIsAddingProduct(false)} className="w-full py-2 text-gray-500 font-bold text-[10px] uppercase tracking-widest hover:text-white transition-colors">Annulla</button>
          </div>
        </div>
      )}

      {/* Product Edit Modal */}
      {editingProduct && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-sm z-[100] flex items-end sm:items-center justify-center p-4">
          <div className="bg-[#151821] w-full max-w-sm rounded-3xl p-5 border border-white/10 space-y-3">
            <h3 className="text-lg font-bold">Modifica Prodotto</h3>
            
            {/* Name */}
            <div>
               <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Nome Prodotto</label>
               <input 
                  value={editingProduct.name} 
                  onChange={e => setEditingProduct({...editingProduct, name: e.target.value})}
                  className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none text-sm transition-colors"
               />
            </div>

            {/* Brand */}
            <div>
               <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Marca</label>
               <input 
                  placeholder="Marca"
                  value={editingProduct.brand || ''} 
                  onChange={e => setEditingProduct({...editingProduct, brand: e.target.value})}
                  className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none text-sm transition-colors"
               />
            </div>

            {/* Cat & Loc */}
             <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Categoria</label>
                <select 
                  value={editingProduct.category}
                  onChange={e => setEditingProduct({...editingProduct, category: e.target.value})}
                  className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none text-sm appearance-none transition-colors"
                >
                  {categories.map(c => <option key={c} value={c}>{c}</option>)}
                </select>
              </div>
              <div>
                <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Posizione</label>
                <select 
                  value={editingProduct.location}
                  onChange={e => setEditingProduct({...editingProduct, location: e.target.value})}
                  className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none text-sm appearance-none transition-colors"
                >
                  {locations.map(l => <option key={l} value={l}>{l}</option>)}
                </select>
              </div>
            </div>

            {/* Qty, Unit, Expiry */}
            <div className="grid grid-cols-2 gap-3">
               <div>
                  <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Quantità</label>
                  <div className="flex gap-2">
                    <input 
                      type="number" 
                      value={editingProduct.quantity} 
                      onChange={e => setEditingProduct({...editingProduct, quantity: Number(e.target.value)})} 
                      className="w-16 bg-white/5 border border-white/10 rounded-xl px-2 py-2.5 outline-none text-center text-sm transition-colors" 
                    />
                    <select 
                      value={editingProduct.unit}
                      onChange={e => setEditingProduct({...editingProduct, unit: e.target.value})}
                      className="flex-1 bg-white/5 border border-white/10 rounded-xl px-2 py-2.5 outline-none text-sm appearance-none transition-colors"
                    >
                      {UNITS.map(u => <option key={u} value={u}>{u}</option>)}
                    </select>
                  </div>
               </div>
               <div>
                   <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Scadenza</label>
                   <input 
                    type="date" 
                    value={editingProduct.expiryDate} 
                    onChange={e => setEditingProduct({...editingProduct, expiryDate: e.target.value})} 
                    className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none text-sm text-gray-400 transition-colors" 
                  />
               </div>
            </div>

            {/* Notes */}
             <div>
                <label className="block text-[10px] font-black uppercase text-gray-500 mb-1 ml-1 tracking-wider">Note</label>
                <textarea 
                  placeholder="Note"
                  value={editingProduct.notes || ''}
                  onChange={e => setEditingProduct({...editingProduct, notes: e.target.value})}
                  className="w-full bg-white/5 border border-white/10 rounded-xl px-3 py-2.5 outline-none text-sm h-16 resize-none transition-colors"
                />
            </div>

            <button onClick={handleUpdateProduct} className="w-full py-3 bg-emerald-500 hover:bg-emerald-600 rounded-xl font-bold text-xs uppercase tracking-wider text-white shadow-lg shadow-emerald-500/20 transition-all">Salva Modifiche</button>
            <button onClick={() => setEditingProduct(null)} className="w-full py-2 text-gray-500 font-bold text-[10px] uppercase tracking-widest hover:text-white transition-colors">Annulla</button>
          </div>
        </div>
      )}

      {/* Shopping Add Modal */}
      {isAddingToShopping && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-sm z-[100] flex items-end sm:items-center justify-center p-4">
          <div className="bg-[#151821] w-full max-w-sm rounded-3xl p-6 border border-white/10 space-y-4">
            <h3 className="text-lg font-bold">Aggiungi alla Spesa</h3>
            
            <div className="space-y-2">
              <input 
                placeholder="Cosa devi comprare?"
                value={newShoppingForm.name}
                onChange={e => setNewShoppingForm({...newShoppingForm, name: e.target.value})}
                className="w-full bg-white/5 border border-white/10 rounded-xl p-3 outline-none"
              />
              <input 
                placeholder="Marca (opzionale)"
                value={newShoppingForm.brand || ''}
                onChange={e => setNewShoppingForm({...newShoppingForm, brand: e.target.value})}
                className="w-full bg-white/5 border border-white/10 rounded-xl p-3 outline-none text-sm"
              />
            </div>

            <div className="flex gap-2">
              <input 
                type="number" 
                placeholder="Qtà"
                value={newShoppingForm.quantity}
                onChange={e => setNewShoppingForm({...newShoppingForm, quantity: Number(e.target.value)})}
                className="w-20 bg-white/5 border border-white/10 rounded-xl p-3 outline-none text-center"
              />
              <select 
                value={newShoppingForm.unit}
                onChange={e => setNewShoppingForm({...newShoppingForm, unit: e.target.value})}
                className="flex-1 bg-white/5 border border-white/10 rounded-xl p-3 outline-none text-sm"
              >
                {UNITS.map(u => <option key={u} value={u}>{u}</option>)}
              </select>
            </div>

            <select 
              value={newShoppingForm.shopId}
              onChange={e => setNewShoppingForm({...newShoppingForm, shopId: e.target.value})}
              className="w-full bg-white/5 border border-white/10 rounded-xl p-3 outline-none text-sm"
            >
              <option value="Tutti">Tutti i negozi</option>
              {shops.map(s => <option key={s.id} value={s.id}>{s.name}</option>)}
            </select>

            <textarea 
              placeholder="Note / Info aggiuntive"
              value={newShoppingForm.notes || ''}
              onChange={e => setNewShoppingForm({...newShoppingForm, notes: e.target.value})}
              className="w-full bg-white/5 border border-white/10 rounded-xl p-3 outline-none text-sm h-20 resize-none"
            />

            <button onClick={handleSaveNewShopping} className="w-full py-3 bg-emerald-500 text-white rounded-xl font-bold text-sm">Aggiungi</button>
            <button onClick={() => setIsAddingToShopping(false)} className="w-full py-2 text-gray-500 font-bold text-xs uppercase tracking-widest">Annulla</button>
          </div>
        </div>
      )}

      {/* Shopping Edit Modal */}
      {editingShoppingItem && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-sm z-[100] flex items-end sm:items-center justify-center p-4">
          <div className="bg-[#151821] w-full max-w-sm rounded-3xl p-6 border border-white/10 space-y-4">
            <h3 className="text-lg font-bold">Modifica Spesa</h3>
            
            <div className="space-y-2">
              <input 
                value={editingShoppingItem.name}
                onChange={e => setEditingShoppingItem({...editingShoppingItem, name: e.target.value})}
                className="w-full bg-white/5 border border-white/10 rounded-xl p-3 outline-none"
              />
              <input 
                placeholder="Marca"
                value={editingShoppingItem.brand || ''}
                onChange={e => setEditingShoppingItem({...editingShoppingItem, brand: e.target.value})}
                className="w-full bg-white/5 border border-white/10 rounded-xl p-3 outline-none text-sm"
              />
            </div>

            <div className="flex gap-2">
              <input 
                type="number" 
                value={editingShoppingItem.quantity}
                onChange={e => setEditingShoppingItem({...editingShoppingItem, quantity: Number(e.target.value)})}
                className="w-20 bg-white/5 border border-white/10 rounded-xl p-3 outline-none text-center"
              />
              <select 
                value={editingShoppingItem.unit}
                onChange={e => setEditingShoppingItem({...editingShoppingItem, unit: e.target.value})}
                className="flex-1 bg-white/5 border border-white/10 rounded-xl p-3 outline-none text-sm"
              >
                {UNITS.map(u => <option key={u} value={u}>{u}</option>)}
              </select>
            </div>

            <select 
              value={editingShoppingItem.shopId}
              onChange={e => setEditingShoppingItem({...editingShoppingItem, shopId: e.target.value})}
              className="w-full bg-white/5 border border-white/10 rounded-xl p-3 outline-none text-sm"
            >
              <option value="Tutti">Tutti i negozi</option>
              {shops.map(s => <option key={s.id} value={s.id}>{s.name}</option>)}
            </select>

            <textarea 
              placeholder="Note"
              value={editingShoppingItem.notes || ''}
              onChange={e => setEditingShoppingItem({...editingShoppingItem, notes: e.target.value})}
              className="w-full bg-white/5 border border-white/10 rounded-xl p-3 outline-none text-sm h-20 resize-none"
            />

            <button onClick={handleUpdateShoppingItem} className="w-full py-3 bg-emerald-500 text-white rounded-xl font-bold text-sm">Salva</button>
            <button onClick={() => setEditingShoppingItem(null)} className="w-full py-2 text-gray-500 font-bold text-xs uppercase tracking-widest">Annulla</button>
          </div>
        </div>
      )}

      {/* Cooking Modal */}
      {cookingProduct && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-sm z-[100] flex items-center justify-center p-4">
          <div className="bg-[#151821] w-full max-w-sm rounded-3xl p-6 border border-white/10 space-y-4 text-center">
            <div className="w-16 h-16 bg-emerald-500/20 rounded-full flex items-center justify-center mx-auto text-3xl">🍳</div>
            <h3 className="text-lg font-bold">Cucina {cookingProduct.name}</h3>
            <p className="text-xs text-gray-400">Trasforma questo ingrediente in un piatto cucinato. Verrà spostato in "Cucinato" e la scadenza estesa di 3 giorni.</p>
            <input 
              placeholder="Nome del piatto (es. Pasta al...)"
              value={newDishName}
              onChange={e => setNewDishName(e.target.value)}
              className="w-full bg-white/5 border border-white/10 rounded-xl p-3 outline-none text-center"
            />
            <button onClick={handleCookTransformation} className="w-full py-3 bg-emerald-500 text-white rounded-xl font-bold text-sm">Conferma</button>
            <button onClick={() => setCookingProduct(null)} className="w-full py-2 text-gray-500 font-bold text-xs uppercase tracking-widest">Annulla</button>
          </div>
        </div>
      )}

      {/* Cart Closure Modal */}
      {isClosingCart && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-sm z-[100] flex items-end sm:items-center justify-center p-4">
          <div className="bg-[#151821] w-full max-w-sm rounded-3xl p-6 border border-white/10 space-y-4">
            <h3 className="text-lg font-bold">Chiudi Carrello: {shops.find(s => s.id === isClosingCart)?.name}</h3>
            
            {cartClosureStep === 'choice' && (
              <div className="grid grid-cols-2 gap-3">
                <button onClick={() => { setIsScanning('receipt'); }} className="p-4 bg-white/5 rounded-2xl border border-white/10 hover:border-emerald-500 transition-all text-center">
                  <span className="text-2xl block mb-2">📸</span>
                  <span className="text-xs font-bold uppercase">Scansiona Scontrino</span>
                </button>
                <button onClick={() => setCartClosureStep('manual_price')} className="p-4 bg-white/5 rounded-2xl border border-white/10 hover:border-emerald-500 transition-all text-center">
                  <span className="text-2xl block mb-2">💶</span>
                  <span className="text-xs font-bold uppercase">Inserisci Totale</span>
                </button>
              </div>
            )}

            {cartClosureStep === 'manual_price' && (
              <div className="space-y-4">
                <input 
                  type="number" 
                  placeholder="Totale speso (€)"
                  value={manualTotalPrice}
                  onChange={e => setManualTotalPrice(e.target.value)}
                  className="w-full bg-white/5 border border-white/10 rounded-xl p-4 text-center text-2xl font-black outline-none"
                />
                <button onClick={() => setCartClosureStep('handle_remaining')} className="w-full py-3 bg-emerald-500 text-white rounded-xl font-bold text-sm">Avanti</button>
              </div>
            )}

            {cartClosureStep === 'handle_remaining' && (
              <div className="space-y-4">
                <p className="text-sm text-gray-400 text-center">Cosa fare con gli articoli non spuntati?</p>
                <div className="space-y-2">
                  <button onClick={() => { setMoveRemainingTo('delete'); finalizeCartClosure(Number(manualTotalPrice)); }} className="w-full p-3 bg-rose-500/10 text-rose-400 border border-rose-500/20 rounded-xl text-sm font-bold">🗑️ Rimuovi dalla lista</button>
                  <button onClick={() => { setMoveRemainingTo('keep'); finalizeCartClosure(Number(manualTotalPrice)); }} className="w-full p-3 bg-white/5 text-white border border-white/10 rounded-xl text-sm font-bold">🛒 Mantieni nel carrello</button>
                  <button onClick={() => { setMoveRemainingTo('Tutti'); finalizeCartClosure(Number(manualTotalPrice)); }} className="w-full p-3 bg-white/5 text-white border border-white/10 rounded-xl text-sm font-bold">📄 Sposta in lista generica</button>
                </div>
              </div>
            )}

            <button onClick={() => { setIsClosingCart(null); setCartClosureStep('choice'); }} className="w-full py-2 text-gray-500 font-bold text-xs uppercase tracking-widest">Annulla</button>
          </div>
        </div>
      )}

      {/* Delete Confirmation */}
      {productToDelete && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-sm z-[100] flex items-center justify-center p-4">
          <div className="bg-[#151821] w-full max-w-sm rounded-3xl p-6 border border-white/10 text-center space-y-4">
            <h3 className="text-lg font-bold">Elimina {productToDelete.name}?</h3>
            <p className="text-sm text-gray-400">Vuoi aggiungere questo prodotto alla lista della spesa prima di eliminarlo?</p>
            <div className="flex gap-2">
              <button onClick={() => confirmDeleteAndAddShopping(true)} className="flex-1 py-3 bg-emerald-500 text-white rounded-xl font-bold text-xs uppercase">Sì, aggiungi</button>
              <button onClick={() => confirmDeleteAndAddShopping(false)} className="flex-1 py-3 bg-rose-500 text-white rounded-xl font-bold text-xs uppercase">No, solo elimina</button>
            </div>
            <button onClick={() => setProductToDelete(null)} className="text-gray-500 font-bold text-xs uppercase tracking-widest mt-2">Annulla</button>
          </div>
        </div>
      )}

      {selectedRecipe && (
        <div className="fixed inset-0 bg-black/90 backdrop-blur-2xl z-[200] flex flex-col p-6 overflow-y-auto">
           <div className="flex justify-between items-center mb-6">
              <h2 className="text-2xl font-black text-white">{selectedRecipe.title}</h2>
              <button onClick={() => setSelectedRecipe(null)} className="p-2 glass rounded-full text-white"><svg className="w-6 h-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M6 18L18 6M6 6l12 12" /></svg></button>
           </div>
           
           <div className="space-y-6">
              {selectedRecipe.youtubeId && (
                <div className="w-full aspect-video rounded-2xl overflow-hidden shadow-2xl border border-white/10 bg-black">
                  <iframe width="100%" height="100%" src={`https://www.youtube.com/embed/${selectedRecipe.youtubeId}`} title="Recipe Video" frameBorder="0" allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture" allowFullScreen></iframe>
                </div>
              )}
              <div className="bg-emerald-500/10 p-4 rounded-2xl border border-emerald-500/20">
                 <h3 className="text-xs font-black uppercase text-emerald-400 mb-2">Istruzioni & Descrizione</h3>
                 <p className="text-sm text-gray-200 leading-relaxed whitespace-pre-line">{selectedRecipe.instructions || selectedRecipe.description}</p>
              </div>
              <div>
                 <h3 className="text-xs font-black uppercase text-gray-500 mb-2 tracking-widest">Ingredienti Mancanti</h3>
                 <div className="grid gap-2">
                    {selectedRecipe.missingIngredients?.map(mi => {
                      const isAdded = shoppingList.some(item => item.name.toLowerCase() === mi.toLowerCase());
                      return (
                        <div key={mi} className={`flex items-center justify-between glass p-3 rounded-xl border-l-4 transition-all ${isAdded ? 'border-emerald-500 bg-emerald-500/5' : 'border-rose-500'}`}>
                           <span className="text-xs font-bold text-gray-300">{mi}</span>
                           <button onClick={() => !isAdded && setShoppingList(prev => [{ id: Math.random().toString(36).substr(2, 9), name: mi, quantity: 1, unit: 'pz', shopId: 'Tutti', isPurchased: false }, ...prev])} 
                             className={`px-3 py-1.5 rounded-lg text-[10px] font-black uppercase transition-all flex items-center gap-1.5 ${isAdded ? 'text-emerald-500 bg-emerald-500/10' : 'text-emerald-500 bg-emerald-500/5 active:scale-95'}`}>
                             {isAdded ? <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="3" d="M5 13l4 4L19 7" /></svg> : '+ Spesa'}
                           </button>
                        </div>
                      );
                    })}
                 </div>
              </div>
           </div>
        </div>
      )}

      {/* Google Lens Style Scanner UI */}
      {isScanning && (
        <div className="fixed inset-0 bg-black z-[200] flex flex-col items-center justify-center p-6 animate-in fade-in">
          <div className="w-full max-w-xs aspect-[3/4] border-4 border-emerald-500/50 rounded-[2.5rem] relative overflow-hidden mb-8 shadow-[0_0_50px_rgba(16,185,129,0.3)] bg-gray-900">
            {/* Camera Video Feed */}
            <video 
              ref={videoRef} 
              autoPlay 
              playsInline 
              muted
              className="w-full h-full object-cover" 
            />
            
            {/* Google Lens Overlays */}
            <div className="absolute inset-0 pointer-events-none">
              {/* Corner Accents */}
              <div className="absolute top-6 left-6 w-12 h-12 border-t-4 border-l-4 border-emerald-500 rounded-tl-2xl opacity-80" />
              <div className="absolute top-6 right-6 w-12 h-12 border-t-4 border-r-4 border-emerald-500 rounded-tr-2xl opacity-80" />
              <div className="absolute bottom-6 left-6 w-12 h-12 border-b-4 border-l-4 border-emerald-500 rounded-bl-2xl opacity-80" />
              <div className="absolute bottom-6 right-6 w-12 h-12 border-b-4 border-r-4 border-emerald-500 rounded-br-2xl opacity-80" />

              {/* Scanning Beam Animation */}
              <div className="absolute left-4 right-4 h-[2px] bg-gradient-to-r from-transparent via-emerald-400 to-transparent shadow-[0_0_15px_rgba(16,185,129,0.8)] animate-[scan_2.5s_infinite_ease-in-out]" style={{
                top: '0%',
              }} />

              {/* Fake Detection Dots (Lens Style) */}
              <div className="absolute top-[30%] left-[40%] w-2 h-2 bg-white rounded-full shadow-[0_0_8px_white] animate-pulse opacity-60" />
              <div className="absolute top-[60%] left-[70%] w-3 h-3 bg-white rounded-full shadow-[0_0_10px_white] animate-pulse opacity-40" />
              <div className="absolute top-[45%] left-[20%] w-1.5 h-1.5 bg-white rounded-full shadow-[0_0_6px_white] animate-pulse opacity-50" />
              
              <div className="absolute inset-0 bg-emerald-500/5 mix-blend-overlay" />
            </div>

            <canvas ref={canvasRef} className="hidden" />
          </div>

          <div className="flex flex-col items-center gap-6">
            <button 
              onClick={captureAndScan} 
              disabled={isAnalyzingImage}
              className="px-12 py-5 bg-emerald-500 text-white rounded-full font-black uppercase text-xs shadow-[0_8px_30px_rgba(16,185,129,0.4)] active:scale-95 transition-all flex items-center gap-3 disabled:opacity-50"
            >
              <svg className="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2.5" d="M3 9a2 2 0 012-2h.93a2 2 0 001.664-.89l.812-1.22A2 2 0 0110.07 4h3.86a2 2 0 011.664.89l.812 1.22A2 2 0 0018.07 7H19a2 2 0 012 2v9a2 2 0 01-2 2H5a2 2 0 01-2-2V9z" /><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2.5" d="M15 13a3 3 0 11-6 0 3 3 0 016 0z" /></svg>
              {isAnalyzingImage ? 'Analisi...' : 'AI Scan'}
            </button>
            
            <button onClick={() => setIsScanning(null)} className="text-gray-400 font-black uppercase text-[11px] tracking-[0.2em] hover:text-white transition-colors">
              Annulla
            </button>
          </div>

          {/* Scanner Styles */}
          <style>{`
            @keyframes scan {
              0% { top: 15%; opacity: 0; }
              10% { opacity: 1; }
              90% { opacity: 1; }
              100% { top: 85%; opacity: 0; }
            }
          `}</style>
        </div>
      )}

      <Assistant inventory={products} />
    </div>
  );
};

const NavBtn: React.FC<{active: boolean, onClick: () => void, label: string, icon: React.ReactNode}> = ({active, onClick, label, icon}) => (
  <button onClick={onClick} className={`flex flex-col items-center gap-0.5 transition-all ${active ? 'text-emerald-500' : 'text-gray-500 hover:text-gray-300'}`}>
    <div className={`p-1.5 rounded-xl transition-all ${active ? 'bg-emerald-500/10 shadow-inner' : ''}`}>{icon}</div>
    <span className="text-[8px] font-black uppercase tracking-wider">{label}</span>
  </button>
);

export default App;
