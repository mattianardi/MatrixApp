
import React, { useState, useEffect, useMemo, useRef } from 'react';
import { Product, ShoppingItem, ProductStatus, Origin, Shop, CartHistory, Recipe } from './types';
import { MOCK_PRODUCTS, MOCK_SHOPPING, CATEGORIES as DEFAULT_CATEGORIES, LOCATIONS as DEFAULT_LOCATIONS, MOCK_SHOPS } from './constants';
import ProductCard from './components/ProductCard';
import Assistant from './components/Assistant';
import { analyzeReceipt, parseVoiceCommand, analyzeProductImage, generateRecipeSuggestions, searchWebRecipes } from './services/geminiService';

const App: React.FC = () => {
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
  
  // Cucina state
  const [cookingProduct, setCookingProduct] = useState<Product | null>(null);
  const [newDishName, setNewDishName] = useState('');

  const [historyFilterDays, setHistoryFilterDays] = useState<number>(30);

  // Forms
  const [newProductForm, setNewProductForm] = useState<Partial<Product> & { shopId?: string }>({
    name: '', brand: '', notes: '', category: 'Altro', location: 'Frigo', expiryDate: '', quantity: 1, unit: 'pz', shopId: 'Tutti'
  });

  const [newShoppingForm, setNewShoppingForm] = useState<Partial<ShoppingItem>>({
    name: '', quantity: 1, unit: 'pz', shopId: 'Tutti', brand: '', reason: ''
  });

  // Cart closure state
  const [isClosingCart, setIsClosingCart] = useState<string | null>(null);
  const [cartClosureStep, setCartClosureStep] = useState<'choice' | 'manual_price' | 'handle_remaining'>('choice');
  const [manualTotalPrice, setManualTotalPrice] = useState<string>('');
  const [moveRemainingTo, setMoveRemainingTo] = useState<string>('delete');

  const [filterCategory, setFilterCategory] = useState('Tutti');
  const [filterLocation, setFilterLocation] = useState('Tutti');
  const [searchQuery, setSearchQuery] = useState('');

  // Chef state
  const [selectedChefIngredients, setSelectedChefIngredients] = useState<string[]>([]);
  const [aiRecipes, setAiRecipes] = useState<Recipe[]>([]);
  const [webRecipes, setWebRecipes] = useState<Recipe[]>([]);
  const [isGeneratingRecipes, setIsGeneratingRecipes] = useState(false);
  const [selectedRecipe, setSelectedRecipe] = useState<Recipe | null>(null);

  const videoRef = useRef<HTMLVideoElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);

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

  // Stock statistics for the dashboard
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
    setNewProductForm({ name: '', brand: '', notes: '', category: 'Altro', location: 'Frigo', expiryDate: '', quantity: 1, unit: 'pz', shopId: 'Tutti' });
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
      unit: newShoppingForm.unit || 'pz', 
      shopId: newShoppingForm.shopId || 'Tutti', 
      isPurchased: false,
      reason: newShoppingForm.reason
    };
    setShoppingList(prev => [item, ...prev]);
    setIsAddingToShopping(false);
    setNewShoppingForm({ name: '', quantity: 1, unit: 'pz', shopId: 'Tutti', brand: '', reason: '' });
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
      ctx?.drawImage(videoRef.current, 0, 0, 400, 400);
      const dataUrl = canvasRef.current.toDataURL('image/jpeg');
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
      } catch (e) { alert("Errore analisi"); } finally { setIsAnalyzingImage(false); }
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

  const ShoppingListView = () => {
    const groupedItems = useMemo(() => {
      const groups: Record<string, { shopName: string; toBuy: ShoppingItem[]; purchased: ShoppingItem[] }> = {};
      shops.forEach(s => { groups[s.id] = { shopName: s.name, toBuy: [], purchased: [] }; });
      groups['Tutti'] = { shopName: '🛒 Carrello Generico', toBuy: [], purchased: [] };
      shoppingList.forEach(item => {
        const groupId = item.shopId || 'Tutti';
        if (!groups[groupId]) groups[groupId] = { shopName: 'Shop Sconosciuto', toBuy: [], purchased: [] };
        if (item.isPurchased) groups[groupId].purchased.push(item);
        else groups[groupId].toBuy.push(item);
      });
      return groups;
    }, [shoppingList]);

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

  const ShoppingItemRow = ({ item }: { item: ShoppingItem }) => (
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
                <svg className="w-16 h-16 mb-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="1" d="M19 11H5m14 0a2 2 0 012 2v6a2 2 0 01-2 2H5a2 2 0 01-2-2v-6a2 2 0 012-2m14 0V9a2 2 0 00-2-2M5 11V9a2 2 0 012-2m0 0V5a2 2 0 012-2h6a2 2 0 012 2v2M7 7h10" /></svg>
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
            <h2 className="text-xl font-bold">Impostazioni</h2>
            
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

            <section className="space-y-4">
              <div className="flex items-center justify-between">
                <h3 className="text-xs font-black uppercase tracking-widest text-gray-500">Tipologie Prodotto</h3>
                <button onClick={() => {
                  const val = prompt("Nuova tipologia:");
                  if (val && !categories.includes(val)) setCategories([...categories, val]);
                }} className="p-1.5 glass rounded-lg text-emerald-500"><svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M12 4v16m8-8H4" /></svg></button>
              </div>
              <div className="flex flex-wrap gap-2">
                {categories.map(c => (
                  <div key={c} className="flex items-center gap-2 glass px-3 py-1.5 rounded-xl border border-white/5">
                    <span className="text-[10px] font-bold uppercase">{c}</span>
                    <button onClick={() => setCategories(categories.filter(x => x !== c))} className="text-rose-500"><svg className="w-3 h-3" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M6 18L18 6M6 6l12 12" /></svg></button>
                  </div>
                ))}
              </div>
            </section>

            <section className="space-y-4">
              <div className="flex items-center justify-between">
                <h3 className="text-xs font-black uppercase tracking-widest text-gray-500">Posti Stock</h3>
                <button onClick={() => {
                  const val = prompt("Nuovo posto stoccaggio:");
                  if (val && !locations.includes(val)) setLocations([...locations, val]);
                }} className="p-1.5 glass rounded-lg text-emerald-500"><svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M12 4v16m8-8H4" /></svg></button>
              </div>
              <div className="grid gap-2">
                {locations.map(l => (
                  <div key={l} className="flex items-center justify-between glass p-3 rounded-xl border border-white/5">
                    <span className="text-sm font-bold">{l}</span>
                    <button onClick={() => setLocations(locations.filter(x => x !== l))} className="text-rose-500 p-1 hover:bg-rose-500/10 rounded-lg"><svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" /></svg></button>
                  </div>
                ))}
              </div>
            </section>
          </div>
        )}
      </main>

      {/* MODALS */}

      {/* Cucina Prodotto Modal */}
      {cookingProduct && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-xl z-[200] flex items-center justify-center p-4">
          <div className="w-full max-w-sm bg-[#151821] rounded-2xl p-6 border border-white/5 shadow-2xl animate-in zoom-in-95">
            <h2 className="text-lg font-bold mb-2">Cosa stai cucinando?</h2>
            <p className="text-[10px] text-gray-400 uppercase tracking-widest mb-4">Usando: {cookingProduct.name}</p>
            <div className="space-y-4">
              <input 
                autoFocus 
                value={newDishName} 
                onChange={e => setNewDishName(e.target.value)} 
                className="w-full bg-white/5 rounded-xl py-3 px-4 text-sm border border-white/10 focus:ring-1 focus:ring-emerald-500 outline-none" 
                placeholder="Nome del piatto (es. Frittata, Sugo...)" 
              />
              <div className="flex gap-3">
                <button onClick={() => { setCookingProduct(null); setNewDishName(''); }} className="flex-1 glass py-3 rounded-xl font-bold text-gray-400 text-xs">Annulla</button>
                <button onClick={handleCookTransformation} disabled={!newDishName} className="flex-2 bg-emerald-500 py-3 rounded-xl font-black uppercase text-[10px] tracking-widest text-white shadow-lg active:scale-95 transition-all disabled:opacity-50">Crea Piatto</button>
              </div>
            </div>
          </div>
        </div>
      )}

      {/* Add To Shopping List */}
      {isAddingToShopping && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-xl z-[140] flex items-center justify-center p-4">
          <div className="w-full max-w-sm bg-[#151821] rounded-2xl p-6 border border-white/5 shadow-2xl animate-in zoom-in-95">
            <div className="flex justify-between items-center mb-5">
              <h2 className="text-lg font-bold">Nuovo Articolo Spesa</h2>
              <button onClick={() => setIsAddingToShopping(false)} className="p-1.5 glass rounded-lg hover:bg-white/10 transition-colors"><svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M6 18L18 6M6 6l12 12" /></svg></button>
            </div>
            <div className="space-y-4">
              <div>
                <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Articolo</label>
                <input autoFocus value={newShoppingForm.name} onChange={e => setNewShoppingForm({...newShoppingForm, name: e.target.value, shopId: suggestShopForProduct(e.target.value)})} className="w-full bg-white/5 rounded-xl py-3 px-4 text-sm border border-white/10 focus:ring-1 focus:ring-emerald-500 outline-none" placeholder="Es. Pane, Latte..." />
              </div>
              <div>
                <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Marca</label>
                <input value={newShoppingForm.brand} onChange={e => setNewShoppingForm({...newShoppingForm, brand: e.target.value})} className="w-full bg-white/5 rounded-xl py-3 px-4 text-sm border border-white/10 outline-none" placeholder="Marca (opzionale)" />
              </div>
              <div className="grid grid-cols-2 gap-3">
                <div>
                  <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Quantità</label>
                  <input type="number" value={newShoppingForm.quantity} onChange={e => setNewShoppingForm({...newShoppingForm, quantity: parseInt(e.target.value) || 1})} className="w-full bg-white/5 rounded-xl py-3 px-4 text-sm border border-white/10" />
                </div>
                <div>
                  <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Unità</label>
                  <select value={newShoppingForm.unit} onChange={e => setNewShoppingForm({...newShoppingForm, unit: e.target.value})} className="w-full bg-white/5 rounded-xl py-3 px-2 text-sm border border-white/10">
                    <option value="pz">pz</option>
                    <option value="confezioni">confezioni</option>
                    <option value="kg">kg</option>
                    <option value="g">g</option>
                    <option value="l">l</option>
                    <option value="ml">ml</option>
                  </select>
                </div>
              </div>
              <div>
                <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Shop</label>
                <select value={newShoppingForm.shopId} onChange={e => setNewShoppingForm({...newShoppingForm, shopId: e.target.value})} className="w-full bg-white/5 rounded-xl py-3 px-2 text-xs border border-white/10">
                  <option value="Tutti">Generico</option>
                  {shops.map(s => <option key={s.id} value={s.id}>{s.name}</option>)}
                </select>
              </div>
              <button onClick={handleSaveNewShopping} className="w-full bg-indigo-600 py-3.5 rounded-xl font-black uppercase tracking-widest text-white text-xs shadow-lg active:scale-95 transition-all">Aggiungi alla Lista</button>
            </div>
          </div>
        </div>
      )}

      {/* Edit Product Modal */}
      {editingProduct && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-xl z-[150] flex items-center justify-center p-4">
          <div className="w-full max-w-sm bg-[#151821] rounded-2xl p-6 border border-white/5 shadow-2xl animate-in zoom-in-95">
             <div className="flex justify-between items-center mb-5">
               <h2 className="text-lg font-bold">Modifica Prodotto</h2>
               <button onClick={() => setEditingProduct(null)} className="p-1.5 glass rounded-lg hover:bg-white/10 transition-colors">
                 <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M6 18L18 6M6 6l12 12" /></svg>
               </button>
             </div>
             <div className="space-y-4">
                <div>
                  <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Nome</label>
                  <input value={editingProduct.name} onChange={e => setEditingProduct({...editingProduct, name: e.target.value})} className="w-full bg-white/5 border border-white/10 rounded-xl py-3 px-4 outline-none text-sm focus:border-emerald-500/50" placeholder="Nome" />
                </div>
                
                <div className="grid grid-cols-2 gap-3">
                   <div>
                     <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Categoria</label>
                     <select value={editingProduct.category} onChange={e => setEditingProduct({...editingProduct, category: e.target.value})} className="w-full bg-white/5 border border-white/10 rounded-xl py-3 px-2 text-xs outline-none">
                       {categories.map(c => <option key={c} value={c}>{c}</option>)}
                     </select>
                   </div>
                   <div>
                     <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Posizione</label>
                     <select value={editingProduct.location} onChange={e => setEditingProduct({...editingProduct, location: e.target.value})} className="w-full bg-white/5 border border-white/10 rounded-xl py-3 px-2 text-xs outline-none">
                       {locations.map(l => <option key={l} value={l}>{l}</option>)}
                     </select>
                   </div>
                </div>

                <div className="grid grid-cols-2 gap-3">
                   <div>
                     <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Quantità</label>
                     <input type="number" value={editingProduct.quantity} onChange={e => setEditingProduct({...editingProduct, quantity: parseInt(e.target.value) || 0})} className="w-full bg-white/5 border border-white/10 rounded-xl py-3 px-4 outline-none text-sm" />
                   </div>
                   <div>
                     <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Unità</label>
                     <select value={editingProduct.unit} onChange={e => setEditingProduct({...editingProduct, unit: e.target.value})} className="w-full bg-white/5 border border-white/10 rounded-xl py-3 px-2 text-sm outline-none">
                       <option value="pz">pz</option>
                       <option value="confezioni">confezioni</option>
                       <option value="kg">kg</option>
                       <option value="g">g</option>
                       <option value="l">l</option>
                       <option value="ml">ml</option>
                       <option value="porzioni">porzioni</option>
                     </select>
                   </div>
                </div>

                <div>
                  <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Data di Scadenza</label>
                  <input type="date" value={editingProduct.expiryDate} onChange={e => setEditingProduct({...editingProduct, expiryDate: e.target.value})} className="w-full bg-white/5 border border-white/10 rounded-xl py-3 px-4 outline-none text-sm" />
                </div>

                <button onClick={handleUpdateProduct} className="w-full bg-emerald-500 py-4 rounded-xl font-black uppercase text-xs tracking-widest text-white shadow-lg active:scale-95 transition-all mt-2">Salva Modifiche</button>
             </div>
          </div>
        </div>
      )}

      {/* Edit Shopping Item Modal */}
      {editingShoppingItem && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-xl z-[150] flex items-center justify-center p-4">
          <div className="w-full max-w-sm bg-[#151821] rounded-2xl p-6 border border-white/5 animate-in zoom-in-95">
             <div className="flex justify-between items-center mb-5">
               <h2 className="text-lg font-bold">Modifica Articolo Spesa</h2>
               <button onClick={() => setEditingShoppingItem(null)} className="p-1.5 glass rounded-lg hover:bg-white/10 transition-colors">
                 <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M6 18L18 6M6 6l12 12" /></svg>
               </button>
             </div>
             <div className="space-y-4">
                <div>
                  <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Articolo</label>
                  <input 
                    value={editingShoppingItem.name} 
                    onChange={e => {
                      const newName = e.target.value;
                      const suggestedShop = editingShoppingItem.shopId === 'Tutti' ? suggestShopForProduct(newName) : editingShoppingItem.shopId;
                      setEditingShoppingItem({...editingShoppingItem, name: newName, shopId: suggestedShop});
                    }} 
                    className="w-full bg-white/5 border border-white/10 rounded-xl py-3 px-4 outline-none text-sm focus:border-indigo-500/50" 
                  />
                </div>
                <div>
                  <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Marca</label>
                  <input value={editingShoppingItem.brand || ''} onChange={e => setEditingShoppingItem({...editingShoppingItem, brand: e.target.value})} className="w-full bg-white/5 border border-white/10 rounded-xl py-3 px-4 outline-none text-sm focus:border-indigo-500/50" placeholder="Marca (opzionale)" />
                </div>
                <div className="grid grid-cols-2 gap-3">
                   <div>
                     <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Quantità</label>
                     <input type="number" value={editingShoppingItem.quantity} onChange={e => setEditingShoppingItem({...editingShoppingItem, quantity: parseInt(e.target.value) || 1})} className="w-full bg-white/5 border border-white/10 rounded-xl py-3 px-4 outline-none text-sm" />
                   </div>
                   <div>
                     <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Unità</label>
                     <select value={editingShoppingItem.unit} onChange={e => setEditingShoppingItem({...editingShoppingItem, unit: e.target.value})} className="w-full bg-white/5 border border-white/10 rounded-xl py-3 px-2 text-sm outline-none">
                       <option value="pz">pz</option>
                       <option value="confezioni">confezioni</option>
                       <option value="kg">kg</option>
                       <option value="g">g</option>
                       <option value="l">l</option>
                       <option value="ml">ml</option>
                     </select>
                   </div>
                </div>
                <div>
                   <label className="text-[10px] font-black uppercase text-gray-500 block mb-1 tracking-widest">Shop</label>
                   <select value={editingShoppingItem.shopId} onChange={e => setEditingShoppingItem({...editingShoppingItem, shopId: e.target.value})} className="w-full bg-white/5 border border-white/10 rounded-xl py-3 px-2 text-sm outline-none">
                     <option value="Tutti">Generico</option>
                     {shops.map(s => <option key={s.id} value={s.id}>{s.name}</option>)}
                   </select>
                </div>
                <button onClick={handleUpdateShoppingItem} className="w-full bg-indigo-500 py-4 rounded-xl font-black uppercase text-xs tracking-widest text-white shadow-lg active:scale-95 transition-all mt-2">Aggiorna Articolo</button>
             </div>
          </div>
        </div>
      )}

      {/* Add To Stock Modal */}
      {isAddingProduct && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-xl z-[140] flex items-center justify-center p-4 overflow-y-auto">
          <div className="w-full max-w-sm bg-[#151821] rounded-2xl p-5 shadow-2xl border border-white/5 animate-in zoom-in-95 max-h-[95vh] overflow-y-auto relative">
            <div className="flex justify-between items-center mb-4">
              <h2 className="text-lg font-bold">Aggiungi allo Stock</h2>
              <button onClick={() => setIsAddingProduct(false)} className="p-1.5 glass rounded-lg"><svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M6 18L18 6M6 6l12 12" /></svg></button>
            </div>
            <div className="space-y-4">
              <div className="flex gap-2">
                 <button onClick={() => setIsScanning('product')} className="flex-1 bg-emerald-500 text-white py-2.5 rounded-xl text-[9px] font-black uppercase tracking-widest shadow-md">Scansiona Lens</button>
                 <button onClick={() => fileInputRef.current?.click()} className="flex-1 glass text-white py-2.5 rounded-xl text-[9px] font-black uppercase tracking-widest">Foto</button>
                 <input type="file" ref={fileInputRef} onChange={handleFileUpload} className="hidden" accept="image/*" />
              </div>
              <div className="grid grid-cols-2 gap-3">
                <div className="col-span-2">
                  <label className="text-[9px] font-black uppercase text-gray-500 tracking-widest block mb-1">Nome Articolo</label>
                  <input value={newProductForm.name} onChange={e => setNewProductForm({...newProductForm, name: e.target.value, shopId: suggestShopForProduct(e.target.value)})} className="w-full bg-white/5 rounded-lg py-2.5 px-3 text-sm outline-none border border-white/10 focus:border-emerald-500/50" />
                </div>
                <div>
                  <label className="text-[9px] font-black uppercase text-gray-500 tracking-widest block mb-1">Scadenza</label>
                  <input type="date" value={newProductForm.expiryDate} onChange={e => setNewProductForm({...newProductForm, expiryDate: e.target.value})} className="w-full bg-white/5 rounded-lg py-2.5 px-2 text-[11px] outline-none border border-white/10" />
                </div>
                <div>
                  <label className="text-[9px] font-black uppercase text-gray-500 tracking-widest block mb-1">Tipologia</label>
                  <select value={newProductForm.category} onChange={e => setNewProductForm({...newProductForm, category: e.target.value})} className="w-full bg-white/5 rounded-lg py-2.5 px-2 text-[11px] outline-none border border-white/10">
                    {categories.map(c => <option key={c} value={c}>{c}</option>)}
                  </select>
                </div>
                <div className="grid grid-cols-2 gap-1">
                   <input type="number" value={newProductForm.quantity} onChange={e => setNewProductForm({...newProductForm, quantity: parseInt(e.target.value) || 0})} className="w-full bg-white/5 rounded-lg py-2.5 px-2 text-sm outline-none border border-white/10" />
                   <input value={newProductForm.unit} onChange={e => setNewProductForm({...newProductForm, unit: e.target.value})} className="w-full bg-white/5 rounded-lg py-2.5 px-2 text-sm outline-none border border-white/10" placeholder="pz" />
                </div>
                <div>
                  <label className="text-[9px] font-black uppercase text-gray-500 tracking-widest block mb-1">Posizione</label>
                  <select value={newProductForm.location} onChange={e => setNewProductForm({...newProductForm, location: e.target.value})} className="w-full bg-white/5 rounded-lg py-2.5 px-2 text-[11px] outline-none border border-white/10">
                    {locations.map(l => <option key={l} value={l}>{l}</option>)}
                  </select>
                </div>
              </div>
              <button onClick={handleSaveNewProduct} className="w-full bg-emerald-500 py-3 rounded-xl font-black uppercase tracking-widest shadow-md text-white text-xs mt-2 active:scale-95 transition-all">Salva nello Stock</button>
            </div>
          </div>
        </div>
      )}

      {/* Delete Confirmation Modal */}
      {productToDelete && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-xl z-[150] flex items-center justify-center p-6">
          <div className="w-full max-w-xs bg-[#151821] rounded-2xl p-6 space-y-4 border border-white/5 shadow-2xl">
             <div className="text-center">
               <h2 className="text-base font-bold">Hai finito {productToDelete.name}?</h2>
               <p className="text-gray-500 text-[11px] mt-1">Vuoi riaggiungerlo alla lista spesa?</p>
             </div>
             <div className="space-y-2">
                <button onClick={() => confirmDeleteAndAddShopping(true)} className="w-full bg-emerald-500 text-white py-3 rounded-xl font-bold active:scale-95 transition-all">Sì, Sposta in Spesa</button>
                <button onClick={() => confirmDeleteAndAddShopping(false)} className="w-full glass text-rose-400 py-3 rounded-xl font-bold active:scale-95 transition-all">Solo Elimina</button>
                <button onClick={() => setProductToDelete(null)} className="w-full text-gray-500 text-[10px] font-black uppercase text-center mt-2">Annulla</button>
             </div>
          </div>
        </div>
      )}

      {/* Cart Closure Flow Modal */}
      {isClosingCart && (
        <div className="fixed inset-0 bg-black/80 backdrop-blur-xl z-[150] flex items-center justify-center p-6">
          <div className="w-full max-w-sm bg-[#151821] rounded-2xl p-6 space-y-5 border border-white/5 shadow-2xl animate-in zoom-in-95">
            {cartClosureStep === 'choice' && (
              <>
                <h2 className="text-lg font-bold text-center">Chiudi Spesa {shops.find(s => s.id === isClosingCart)?.name}</h2>
                <div className="space-y-3">
                  <button onClick={() => setIsScanning('receipt')} className="w-full bg-emerald-500 py-3 rounded-xl font-bold text-white shadow-lg active:scale-95">📸 Scansiona Scontrino</button>
                  <button onClick={() => setCartClosureStep('manual_price')} className="w-full glass py-3 rounded-xl font-bold text-gray-300">✍️ Importo Manuale</button>
                  <button onClick={() => setIsClosingCart(null)} className="w-full text-[10px] font-black uppercase text-gray-500">Annulla</button>
                </div>
              </>
            )}
            {cartClosureStep === 'manual_price' && (
              <div className="space-y-4">
                <h2 className="text-lg font-bold text-center">Importo Totale</h2>
                <input autoFocus type="number" value={manualTotalPrice} onChange={e => setManualTotalPrice(e.target.value)} placeholder="0.00 €" className="w-full bg-white/5 py-4 rounded-xl text-center text-2xl font-black border border-white/10 outline-none" />
                <button onClick={() => setCartClosureStep('handle_remaining')} disabled={!manualTotalPrice} className="w-full bg-emerald-500 py-3 rounded-xl font-bold disabled:opacity-50">Continua</button>
              </div>
            )}
            {cartClosureStep === 'handle_remaining' && (
              <div className="space-y-4">
                <h2 className="text-lg font-bold text-center">Articoli mancanti?</h2>
                <div className="space-y-2">
                  {['delete', 'keep', ...shops.filter(s => s.id !== isClosingCart).map(s => s.id)].map(opt => (
                    <label key={opt} className="flex items-center gap-3 glass p-3 rounded-xl cursor-pointer border border-white/5">
                      <input type="radio" checked={moveRemainingTo === opt} onChange={() => setMoveRemainingTo(opt)} className="accent-emerald-500" />
                      <span className="text-[11px] font-bold">
                        {opt === 'delete' ? 'Elimina definit.' : opt === 'keep' ? 'Lascia qui' : `Sposta su ${shops.find(s => s.id === opt)?.name}`}
                      </span>
                    </label>
                  ))}
                </div>
                <button onClick={() => finalizeCartClosure(parseFloat(manualTotalPrice))} className="w-full bg-emerald-500 py-4 rounded-xl font-black uppercase text-xs shadow-xl text-white">Salva Spesa</button>
              </div>
            )}
          </div>
        </div>
      )}

      {/* Recipe Details Modal */}
      {selectedRecipe && (
        <div className="fixed inset-0 bg-black/90 backdrop-blur-2xl z-[200] flex flex-col p-6 overflow-y-auto">
           <div className="flex justify-between items-center mb-6">
              <h2 className="text-2xl font-black text-white">{selectedRecipe.title}</h2>
              <button onClick={() => setSelectedRecipe(null)} className="p-2 glass rounded-full text-white"><svg className="w-6 h-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M6 18L18 6M6 6l12 12" /></svg></button>
           </div>
           
           <div className="space-y-6">
              {/* YouTube Player Integration */}
              {selectedRecipe.youtubeId && (
                <div className="w-full aspect-video rounded-2xl overflow-hidden shadow-2xl border border-white/10 bg-black">
                  <iframe 
                    width="100%" 
                    height="100%" 
                    src={`https://www.youtube.com/embed/${selectedRecipe.youtubeId}`} 
                    title="Recipe Video" 
                    frameBorder="0" 
                    allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture" 
                    allowFullScreen
                  ></iframe>
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
                           <button 
                             onClick={() => {
                               if (!isAdded) {
                                 setShoppingList(prev => [{ id: Math.random().toString(36).substr(2, 9), name: mi, quantity: 1, unit: 'pz', shopId: 'Tutti', isPurchased: false }, ...prev]);
                               }
                             }} 
                             className={`px-3 py-1.5 rounded-lg text-[10px] font-black uppercase transition-all flex items-center gap-1.5 ${isAdded ? 'text-emerald-500 bg-emerald-500/10' : 'text-emerald-500 bg-emerald-500/5 active:scale-95 hover:bg-emerald-500/20'}`}
                           >
                             {isAdded ? (
                               <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="3" d="M5 13l4 4L19 7" /></svg>
                             ) : '+ Spesa'}
                           </button>
                        </div>
                      );
                    })}
                    {(!selectedRecipe.missingIngredients || selectedRecipe.missingIngredients.length === 0) && (
                      <div className="flex items-center gap-2 text-emerald-400 font-bold p-4 glass rounded-xl border border-emerald-500/20">
                        <svg className="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M5 13l4 4L19 7" /></svg>
                        <p className="text-xs uppercase tracking-widest">Hai tutto il necessario!</p>
                      </div>
                    )}
                 </div>
              </div>
           </div>
        </div>
      )}

      {/* Navigation Bar */}
      <nav className="fixed bottom-0 left-0 right-0 h-16 bg-[#0a0b10]/95 border-t border-white/5 flex items-center justify-around px-2 z-[60] max-w-xl mx-auto backdrop-blur-xl">
        <NavBtn active={activeTab === 'home'} onClick={() => setActiveTab('home')} label="Stock" icon={<svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M3 12l2-2m0 0l7-7 7 7M5 10v10a1 1 0 001 1h3m10-11l2 2m-2-2v10a1 1 0 01-1 1h-3m-6 0a1 1 0 001-1v-4a1 1 0 011-1h2a1 1 0 011 1v4a1 1 0 001 1m-6 0h6" /></svg>} />
        <NavBtn active={activeTab === 'shopping'} onClick={() => setActiveTab('shopping')} label="Spesa" icon={<svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M16 11V7a4 4 0 00-8 0v4M5 9h14l1 12H4L5 9z" /></svg>} />
        <NavBtn active={activeTab === 'chef'} onClick={() => setActiveTab('chef')} label="Chef" icon={<svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M13 10V3L4 14h7v7l9-11h-7z"/></svg>} />
        <NavBtn active={activeTab === 'history'} onClick={() => setActiveTab('history')} label="Storico" icon={<svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M12 8v4l3 3m6-3a9 9 0 11-18 0 9 9 0 0118 0z" /></svg>} />
        <NavBtn active={activeTab === 'settings'} onClick={() => setActiveTab('settings')} label="Opzioni" icon={<svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.065 2.572c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.572 1.065c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.065-2.572c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z" /><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z" /></svg>} />
      </nav>

      {/* Fullscreen Scan UI */}
      {isScanning && (
        <div className="fixed inset-0 bg-black z-[200] flex flex-col items-center justify-center p-6">
          <div className="w-full max-w-xs aspect-[3/4] border-4 border-emerald-500 rounded-2xl relative overflow-hidden mb-8 shadow-2xl">
            <video ref={videoRef} autoPlay playsInline className="w-full h-full object-cover" />
            <canvas ref={canvasRef} width="400" height="400" className="hidden" />
          </div>
          <button onClick={captureAndScan} className="px-10 py-4 bg-emerald-500 text-white rounded-xl font-black uppercase text-xs shadow-xl active:scale-95 transition-all">AI Scan</button>
          <button onClick={() => setIsScanning(null)} className="text-gray-500 mt-6 font-bold uppercase text-[10px] tracking-widest">Annulla</button>
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
